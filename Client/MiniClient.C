#include <boost/asio.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <iostream>
#include <sstream>
#include <string>

using namespace boost;
namespace po = boost::program_options;

class SyncTCPClient {
public:
	SyncTCPClient(const std::string& raw_ip_address,
		unsigned short port_num) :
		m_ep(asio::ip::address::from_string(raw_ip_address),
		port_num),
		m_sock(m_ios) {

		m_sock.open(m_ep.protocol());
	}

	void connect() {
		m_sock.connect(m_ep);
	}

	void close() {
		m_sock.shutdown(
			boost::asio::ip::tcp::socket::shutdown_both);
		m_sock.close();
	}

	std::string emulateLongComputationOp(
		unsigned int duration_sec) {

		std::string request = "EMULATE_LONG_COMP_OP "
			+ std::to_string(duration_sec)
			+ "\n";

		sendRequest(request);
		return receiveResponse();
	};

private:
	void sendRequest(const std::string& request) {
		asio::write(m_sock, asio::buffer(request));
	}

	std::string receiveResponse() {
		asio::streambuf buf;
		asio::read_until(m_sock, buf, '\n');

		std::istream input(&buf);

		std::string response;
		std::getline(input, response);

		return response;
	}

private:
	asio::io_service m_ios;

	asio::ip::tcp::endpoint m_ep;
	asio::ip::tcp::socket m_sock;
};

int main(int argc, char* argv[])
{
	//=============================================
	// command line options
	std::string app_name = boost::filesystem::basename(argv[0]);
	std::string raw_ip_address;
	unsigned short port_num;

	std::stringstream ss_help_header;
	ss_help_header << "Command line options. \n" <<
		"Calling the applications is done with the command:\n" <<
	    "\t> " << app_name << "[options]" <<
		"Options";

	program_options::options_description desc(ss_help_header.str());
    desc.add_options()
      ("help,h", "This help text.")
      ("server-address,s", po::value<std::string>(&raw_ip_address)->default_value("127.0.0.1"), "Server address")
      ("port,p", po::value<unsigned short>(&port_num)->default_value(1138), "Port number");

	po::variables_map vm;
	try {
		po::store(parse_command_line(argc, argv, desc), vm);
		po::notify(vm);

		if (vm.count("help")) {
			std::cout << desc << '\n';
			return 0;
		}
		if (vm.count("server-address"))
			std::cout << "Server address set to: " << vm["server-address"].as<std::string>() << '\n';
		if (vm.count("port"))
			std::cout << "Port number set to: " << vm["port"].as<unsigned short>() << '\n';
	}
	catch(boost::program_options::error& e)
	{ 
		std::cerr << "COMMAND LINE ERROR: " << e.what() << std::endl << std::endl; 
	} 

/*
	const std::string raw_ip_address = "127.0.0.1";
	const unsigned short port_num = 1138;
*/

	//=============================================
	// client logic

	std::cout << "TCP synchronous client application. Sending socket to "
		<< raw_ip_address << " on port " << port_num
		<< std::endl;

	try {
		SyncTCPClient client(raw_ip_address, port_num);

		// Sync connect.
		client.connect();

		std::cout << "Sending request to the server... "
			<< std::endl;

		std::string response =
			client.emulateLongComputationOp(10);

		std::cout << "Response received: " << response
			<< std::endl;

		// Close the connection and free resources.
		client.close();
	}
	catch (system::system_error &e) {
		std::cout << "Error occured! Error code = " << e.code()
			<< ". Message: " << e.what()
			<< std::endl;

		return e.code().value();
	}

	return 0;
}