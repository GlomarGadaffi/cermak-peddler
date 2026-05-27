#include <iostream>
#include "SipServer.hpp"
#include "HttpServer.hpp"
#include "IPHelper.hpp"
#include "cxxopts.hpp"

int main(int argc, char** argv)
{
	cxxopts::Options options("SipServer", "Open source server for handling voip calls based on sip.");

	options.add_options()
		("h,help", "Print usage")
		("i,ip",   "Sip server ip",   cxxopts::value<std::string>()->default_value("0.0.0.0"))
		("p,port", "Sip server port.", cxxopts::value<int>()->default_value(std::to_string(5060)))
		("w,web",  "HTTP dashboard port.", cxxopts::value<int>()->default_value(std::to_string(8080)));

	auto result = options.parse(argc, argv);

	if (result.count("help"))
	{
		std::cout << options.help() << std::endl;
		return 0;
	}

	try
	{
		std::string bindIp = result["ip"].as<std::string>();
		int         port   = result["port"].as<int>();
		int         webPort = result["web"].as<int>();

		std::string lanIp = bindIp;
		if (bindIp == "0.0.0.0")
		{
			lanIp = getPrimaryLocalIP();
		}

		SipServer server(bindIp, port);
		std::cout << "SIP server started on " << bindIp << ":" << port << " (LAN IP: " << lanIp << ")\n";

		// Start the CGA CRT web dashboard
		HttpServer http(bindIp, webPort, server.getHandler());
		http.start();
		std::cout << "CGA CRT Dashboard: http://" << (bindIp == "0.0.0.0" ? "localhost" : bindIp) << ":" << webPort << "/\n";
		if (bindIp == "0.0.0.0")
		{
			std::cout << "Remote LAN Access:  http://" << lanIp << ":" << webPort << "/\n";
		}

		std::cout << "Press ENTER to stop...\n";
		std::cin.get();
	}
	catch (const cxxopts::OptionException& e)
	{
		std::cerr << "Option error: " << e.what() << "\nPlease provide a valid --ip and --port.\n";
		return 1;
	}
	catch (const std::exception& e)
	{
		std::cerr << "Fatal error: " << e.what() << '\n';
		return 1;
	}
	return 0;
}
