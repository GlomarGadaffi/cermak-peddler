#ifndef IDGEN_HPP
#define IDGEN_HPP

#include <string>
#include <random>

class IDGen
{
public:
    IDGen() = delete;

    static std::string GenerateID(int len)
    {
        // thread_local engine avoids both seeding-once-globally and data races
        thread_local std::mt19937 rng{std::random_device{}()};
        thread_local std::uniform_int_distribution<int> dist{
            0, static_cast<int>(sizeof(alphanum) - 2)};

        std::string id;
        id.reserve(len);
        for (int i = 0; i < len; ++i)
        {
            id += alphanum[dist(rng)];
        }
        return id;
    }

private:
    static constexpr char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
};

#endif
