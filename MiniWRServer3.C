#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <atomic>
#include <memory>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

using namespace boost;
namespace po = boost::program_options;

// Includes for OpenFOAM
#include "argList.H"
#include "Time.H"
#include "fvMesh.H"
#include "interpolation.H"

// Includes for GeographicLib (http://geographiclib.sourceforge.net/html/index.html)
// https://en.wikipedia.org/wiki/Universal_Transverse_Mercator_coordinate_system
#include <GeographicLib/UTMUPS.hpp>

using namespace Foam;
using namespace GeographicLib;

//===============================================
// GLOBALS


//===============================================
// Server logic

std::string readLineFromSocket(asio::ip::tcp::socket& sock) {
	asio::streambuf buf;
	// Synchronously read data from the socket until
	// '\n' symbol is encountered.
	asio::read_until(sock, buf, '\n');
	std::string message;
	// Because buffer 'buf' may contain some other data
	// after '\n' symbol, we have to parse the buffer and
	// extract only symbols before the delimiter.
	std::istream input_stream(&buf);
	std::getline(input_stream, message);
	return message;
}

class Session
{
	public:
		Session(
			boost::asio::io_service& io_service,
			const Foam::fvMesh *mesh_ptr,
			const volVectorField *U_ptr)
			: socket_(io_service), mesh_ptr_(mesh_ptr),	U_ptr_(U_ptr)
		{
		}
		//-----------------------------------------------------------------------------------------------
		asio::ip::tcp::socket& socket() {
			return socket_;
		}
		//-----------------------------------------------------------------------------------------------
		// Read headers from client and then handle_read
		void start() {
			// init interpolator (must be one per session):
			interpU_ = interpolation< vector >::New("cellPoint",*U_ptr_);

			socket_.async_read_some(
				asio::buffer(mblebuff_),
				boost::bind(
					&Session::handle_read,
					this,
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred)
				);

			sbuff_.commit(boost::asio::placeholders::bytes_transferred);

			// Read from the input sequence and consume the read data.
			// The input sequence is empty
			std::istream input(&sbuff_);
			std::string message;
			// read a line from the streambuf object
			// std::getline(input, message); 
			// input >> message;

			// Clear EOF bit.
    		input.clear();

			std::cout << "\n\n[START] byte_transferred: " << boost::asio::placeholders::bytes_transferred << std::endl;
			std::cout << "\n\n[START] message: " << message << std::endl;

		}

	private:
		//-----------------------------------------------------------------------------------------------
		// Process input, interpolate, process result, write to client and then handle_write
		void handle_read(const boost::system::error_code& error, size_t bytes_transferred)
		{

			if (!error) {
				std::istream input(&sbuff_);
				std::string message;
				// read a line from the streambuf object
				// std::getline(input, message); 
				input >> message;
				// Clear EOF bit.
				input.clear();
				std::cout << "\n\nhandle_read: " << message << std::endl;

				// TODO: parse data here

				// Write
				boost::asio::async_write(socket_,
					boost::asio::buffer("pippo", bytes_transferred),
					boost::bind(&Session::handle_write, this, boost::asio::placeholders::error)
				);
			}
			else {
				delete this;
			}

		}
		//-----------------------------------------------------------------------------------------------
		// Start reading again and then handle_read --> loop until End Of Data
		void handle_write(const boost::system::error_code& error)
		{
			if (!error) {
				std::cout << "\n\nhandle_write ... " << std::endl;
				socket_.async_read_some(
					asio::buffer(mblebuff_),
					boost::bind(&Session::handle_read,
					this,
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred)
				);
			}
			else {
				delete this;
			}
		}

		//-----------------------------------------------------------------------------------------------
		asio::ip::tcp::socket socket_;
		asio::streambuf sbuff_;
		asio::streambuf::mutable_buffers_type mblebuff_ = sbuff_.prepare(512);
		const Foam::fvMesh *mesh_ptr_;
		const volVectorField *U_ptr_;
		autoPtr< interpolation< vector > > interpU_;
};

class Server
{
	public:
		Server(
			boost::asio::io_service& io_service,
			short port, const Foam::fvMesh *mesh_ptr, const volVectorField *U_ptr
			) : io_service_(io_service),
				acceptor_(io_service, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)),
				mesh_ptr_(mesh_ptr),
				U_ptr_(U_ptr)
		{
			start_accept();
		}			
		//-----------------------------------------------------------------------------------------------
		void start_accept()	{
			Session* new_session = new Session(io_service_, mesh_ptr_, U_ptr_);
			acceptor_.async_accept(
				new_session->socket(),
				boost::bind(
					&Server::handle_accept,
					this,
					new_session,
					boost::asio::placeholders::error)
				);
		}
		//-----------------------------------------------------------------------------------------------
		void handle_accept(Session* new_session, const boost::system::error_code& error) {
			if (!error)
				new_session->start();
			else
				delete new_session;

			start_accept();
		}
		//-----------------------------------------------------------------------------------------------
		boost::asio::io_service& io_service_;
		asio::ip::tcp::acceptor acceptor_;
		const Foam::fvMesh *mesh_ptr_;
		const volVectorField *U_ptr_;
};

//===============================================
// Server launcher

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
	    "\t> $FOAM_USER_APPBIN/" << app_name << " [options]\n" <<
		"Options";

	program_options::options_description desc(ss_help_header.str());
    desc.add_options()
      ("help,h", "This help text.")
      ("port,p", po::value<unsigned short>(&port_num)->default_value(1025), "Port number");

	po::variables_map vm;

	try {
		po::store(parse_command_line(argc, argv, desc), vm);
		po::notify(vm);

		if (vm.count("help")) {
			std::cout << desc << '\n';
			return 0;
		}
		if (vm.count("port"))
			std::cout << "Port number set to: " << vm["port"].as<unsigned short>() << '\n';
	}
	catch(boost::program_options::error& e)
	{ 
		std::cerr << "COMMAND LINE ERROR: " << e.what() << std::endl << std::endl; 
	} 

	//=============================================
	// main program logic

	try {

		//=============================================
		// Reading OpenFOAM mesh

		// setRootCase.H
    	Foam::argList args(argc, argv, false, false); // checkArgs = true, bool checkOpts = true, 

		if (!args.checkRootCase()) {
			Foam::FatalError.exit();
		}

		// createTime.H
		std::cout << "Create time\n" << std::endl;
		//read information from system/controlDict: mind for "startFrom latestTime;" entry
		Foam::Time runTime(Foam::Time::controlDictName, args);

		// createMesh.H
		std::cout << "\nCreate mesh for time = " << runTime.timeName() << std::endl;

		Foam::fvMesh mesh(
			Foam::IOobject(
				Foam::fvMesh::defaultRegion,
				runTime.timeName(),
				runTime,
				Foam::IOobject::MUST_READ)
			);

		// load field U:
		std::cout << "Reading field U\n" << std::endl;
		volVectorField U(
			IOobject(
				"U",
				runTime.timeName(),
				mesh,
				IOobject::MUST_READ,
				IOobject::AUTO_WRITE),
			mesh);

		// create pointers
		const Foam::fvMesh *mesh_ptr = &mesh;
		const volVectorField *U_ptr = &U;

		//=============================================
		// Server logic

		std::cout << "TCP asynchronous server listening on port "
			<< port_num << std::endl;
		
		boost::asio::io_service io_service;
		Server s(io_service, port_num, mesh_ptr, U_ptr);
		io_service.run();
	}
	catch (system::system_error &e) {
		std::cout << "Error occured! Error code = "
			<< e.code() << ". Message: "
			<< e.what() << std::endl;
	}
	catch (std::exception& e) {
		std::cerr << "Exception: " << e.what() << "\n";
	}

	return 0;
}
