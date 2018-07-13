/*
On Windows System for Linux (WSL), ubuntu, 

 - Install Boost libraries

 - In bash shell, build with:

> g++ -std=c++11 -m64 -Dlinux64 -DWM_ARCH_OPTION=64 -DWM_DP -DWM_LABEL_SIZE=32 \
    -O3  -DNoRepository -ftemplate-depth-100 \
	-fPIC -g -Wno-unused-local-typedefs \
	-c MiniWRServer.C -o MiniWRServer.o

> g++ -std=c++11 -m64 -Dlinux64 -DWM_ARCH_OPTION=64 -DWM_DP -DWM_LABEL_SIZE=32 \
    -O3  -DNoRepository -ftemplate-depth-100 \
	-fPIC -g -Wno-unused-local-typedefs -Xlinker --add-needed -Xlinker --no-as-needed \
	MiniWRServer.o \
	-lboost_system -lboost_program_options -lboost_filesystem -ldl -lm \
	-o ./MiniWRServer

- In bash shell, run as follows:

> ./MiniWRServer -p 1138

- In a different bash shell, run JSBSim as follows

/path-to-JSBSim-root> JSBSim --script=scripts/C1723.xml --logdirectivefile=socket_oi.xml

Where:

<!-- /path-to-JSBSim-root/socket_oi.xml -->
<output name="localhost" type="SOCKET" port="1138" rate="20" action="WAIT_SOCKET_REPLY" >
  <property> position/lat-gc-deg </property>
  <property> position/long-gc-deg </property>
  <property> position/h-agl-meters </property>
</output>

*/
#include <iostream>
#include <sstream>
#include <string>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/make_shared.hpp>

using namespace boost;
namespace po = boost::program_options;

//===============================================
// GLOBALS

//===============================================
// FUNCTIONS

void writeToSocket(asio::ip::tcp::socket &sock, std::string buf)
{
	std::size_t total_bytes_written = 0;
	// Run writing loop until all data is written to the socket.
	while (total_bytes_written != buf.length())
	{
		std::cout << "Sending bytes to output socket on host " << sock.remote_endpoint().address().to_string() << ", port " << sock.remote_endpoint().port() << std::endl;
		total_bytes_written += sock.write_some(
			asio::buffer(buf.c_str() + total_bytes_written,
						 buf.length() - total_bytes_written));
	}
	std::cout << "Total bytes written " << total_bytes_written << std::endl;
}

std::string readLineFromSocket(asio::ip::tcp::socket &sock)
{
	// object used to collect inbound data
	asio::streambuf sbuff;
	// read inbound data
	std::size_t total_bytes_read = 0;
	total_bytes_read += asio::read_until(sock, sbuff, '\n');
	// extract a line from buffer
	std::istream str(&sbuff); 
	std::string inbound_msg;
	std::getline(str, inbound_msg);
	std::cout << "[Session::start] Read:---" << std::endl
			  << inbound_msg << std::endl 
			  << "---" << std::endl;
	std::cout << "Total bytes read: " << total_bytes_read << std::endl;
	std::cout << "Line length: " << inbound_msg.size() << std::endl;
	return inbound_msg;
}

//===============================================

int main(int argc, char *argv[])
{
	//=============================================
	// command line options

	std::string app_name = boost::filesystem::basename(argv[0]);
	unsigned short input_port_num;
	unsigned short output_port_num;

	std::stringstream ss_help_header;
	ss_help_header << "Command line options. \n"
				   << "Calling the applications is done with the command:\n"
				   << "\t> $FOAM_USER_APPBIN/" << app_name << " [options]\n"
				   << "Options";

	program_options::options_description desc(ss_help_header.str());
	desc.add_options()("help,h", "This help text.")("input-port,i", po::value<unsigned short>(&input_port_num)->default_value(1025), "Input port number")("output-port,o", po::value<unsigned short>(&output_port_num)->default_value(1026), "Output port number");

	po::variables_map vm;

	try
	{
		po::store(parse_command_line(argc, argv, desc), vm);
		po::notify(vm);

		if (vm.count("help"))
		{
			std::cout << desc << '\n';
			return 0;
		}
		if (vm.count("input-port"))
		{
			std::cout << "Input port number set to: " << vm["input-port"].as<unsigned short>() << '\n';
		}
		if (vm.count("output-port"))
		{
			std::cout << "Output port number set to: " << vm["output-port"].as<unsigned short>() << '\n';
		}
	}
	catch (boost::program_options::error &e)
	{
		std::cerr << "COMMAND LINE ERROR: " << e.what() << std::endl
				  << std::endl;
	}

	//=============================================
	// main program logic

	std::string raw_ip_address = "127.0.0.1";
	try
	{
		
		asio::io_service ios;

		/*
		// Endpoint inbound connection
		asio::ip::tcp::endpoint
			ep_inbound(asio::ip::address::from_string(raw_ip_address), input_port_num);
		// Allocating and opening the socket
		asio::ip::tcp::socket sock_inbound(ios, ep_inbound.protocol());
		sock_inbound.connect(ep_inbound);
		std::cout << "Socket for inbound data connected" << std::endl;
		// Reading inbound data
		std::string message_inbound = readLineFromSocket(sock_inbound);
		*/

		// Endpoint outbound connection
		asio::ip::tcp::endpoint
			ep_outbound(asio::ip::address::from_string(raw_ip_address), output_port_num);
		// Allocating and opening the socket
		asio::ip::tcp::socket sock_outbound(ios, ep_outbound.protocol());
		sock_outbound.connect(ep_outbound);
		std::cout << "Socket for outbound data connected" << std::endl;

		// Allocating and filling the buffer for output
		std::string message_outbound = ">>>>>> abcdefghi <<<<<<";

		// Write the buffer content to outbound socket
		writeToSocket(sock_outbound, message_outbound);
	}
	catch (system::system_error &e)
	{
		std::cout << "Error occured! Error code = "
				  << e.code() << ". Message: "
				  << e.what() << std::endl;
	}
	catch (std::exception &e)
	{
		std::cerr << "Exception: " << e.what() << "\n";
	}

	return 0;
}
