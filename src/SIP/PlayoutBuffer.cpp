#include "PlayoutBuffer.hpp"
#include <cstring>
#include <algorithm>

PlayoutBuffer::PlayoutBuffer(size_t maxSamples)
	: _maxSamples(maxSamples)
{
	_buffer.resize(_maxSamples, 0);
}

size_t PlayoutBuffer::write(const int16_t* samples, size_t count)
{
	if (samples == nullptr || count == 0)
	{
		return 0;
	}

	std::lock_guard<std::mutex> lock(_mutex);

	// If count is larger than the total buffer capacity, clamp to max capacity
	if (count > _maxSamples)
	{
		_overruns.fetch_add(1, std::memory_order_relaxed);
		samples += (count - _maxSamples);
		count = _maxSamples;
	}

	// Check if this write causes an overrun (requires dropping oldest samples)
	if (_count + count > _maxSamples)
	{
		size_t overflow = (_count + count) - _maxSamples;
		_overruns.fetch_add(1, std::memory_order_relaxed);

		// Drop the oldest samples by advancing the read pointer
		_readPtr = (_readPtr + overflow) % _maxSamples;
		_count = _maxSamples - count;
	}

	// Write samples into the circular buffer
	for (size_t i = 0; i < count; ++i)
	{
		_buffer[_writePtr] = samples[i];
		_writePtr = (_writePtr + 1) % _maxSamples;
	}
	_count += count;

	return count;
}

bool PlayoutBuffer::read(int16_t* out, size_t count)
{
	if (out == nullptr || count == 0)
	{
		return false;
	}

	std::lock_guard<std::mutex> lock(_mutex);

	// Adaptive latency drain. The buffer used to be a pure FIFO: standing latency floated to
	// wherever producer/consumer rates settled and then only ever GREW toward the ceiling —
	// locking in 200-400 ms of mouth-to-ear delay for the whole call. Instead, keep the
	// depth near _targetDepth: if it has drifted more than one read above target, drop the
	// oldest excess so the delay walks back down. The drop is capped at one read (`count`) so
	// it converges as ≤20 ms micro-skips (mostly in inter-word gaps) rather than one audible
	// gap. This actively MINIMISES latency instead of letting it accumulate.
	const size_t target = _targetDepth.load(std::memory_order_relaxed);
	if (_count > target + 2 * count)
	{
		size_t excess = _count - (target + count);          // amount above the high-water mark
		size_t drop   = (excess < count) ? excess : count;  // at most one frame per read
		_readPtr = (_readPtr + drop) % _maxSamples;
		_count  -= drop;
	}

	bool success = true;
	size_t readCount = count;

	if (_count < count)
	{
		_underruns.fetch_add(1, std::memory_order_relaxed);
		readCount = _count;
		success = false;
	}

	// Read available samples
	for (size_t i = 0; i < readCount; ++i)
	{
		out[i] = _buffer[_readPtr];
		_readPtr = (_readPtr + 1) % _maxSamples;
	}
	_count -= readCount;

	// Fill any remaining samples with low-amplitude comfort noise
	if (readCount < count)
	{
		for (size_t i = readCount; i < count; ++i)
		{
			// Low-overhead LCG pseudo-random noise generator (amplitude range -20 to 20)
			static uint32_t seed = 123456789;
			seed = seed * 1664525 + 1013904223;
			int16_t noise = static_cast<int16_t>((seed % 41) - 20);
			out[i] = noise;
		}
	}

	return success;
}

size_t PlayoutBuffer::getLength() const
{
	std::lock_guard<std::mutex> lock(_mutex);
	return _count;
}

void PlayoutBuffer::clear()
{
	std::lock_guard<std::mutex> lock(_mutex);
	_readPtr = 0;
	_writePtr = 0;
	_count = 0;
	std::fill(_buffer.begin(), _buffer.end(), 0);
	_underruns.store(0, std::memory_order_relaxed);
	_overruns.store(0, std::memory_order_relaxed);
}

void PlayoutBuffer::setTargetDepth(size_t samples)
{
	_targetDepth.store(samples, std::memory_order_relaxed);
}
