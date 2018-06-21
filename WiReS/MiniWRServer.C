/*
On Windows System for Linux (WSL), ubuntu, 

 - Install Boos libraries

 - In bash shell, build with:

> g++ -std=c++11 -m64 -Dlinux64 -DWM_ARCH_OPTION=64 -DWM_DP -DWM_LABEL_SIZE=32 \
    -O3  -DNoRepository -ftemplate-depth-100 \
	-fPIC -g -Wno-unused-local-typedefs \
	-c MiniWRServer.C 
	-o Make/linux64GccDPInt32Opt/MiniWRServer.o
> g++ -std=c++11 -m64 -Dlinux64 -DWM_ARCH_OPTION=64 -DWM_DP -DWM_LABEL_SIZE=32 \
    -O3  -DNoRepository -ftemplate-depth-100 \
	-fPIC -g -Wno-unused-local-typedefs -Xlinker --add-needed -Xlinker --no-as-needed \
	Make/linux64GccDPInt32Opt/MiniWRServer.o 
	-lboost_system -lboost_program_options -lboost_filesystem -ldl \
	-lm -o ./MiniWRServer

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

using namespace boost;
namespace po = boost::program_options;

/*
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
*/

//===============================================
// GLOBALS


//===============================================
// Server logic

class Session
{
	public:
		Session(boost::asio::io_service& io_service	/*, const Foam::fvMesh *mesh_ptr, const volVectorField *U_ptr */ )
			: socket_(io_service) // , mesh_ptr_(mesh_ptr), U_ptr_(U_ptr)
		{
		}

		asio::ip::tcp::socket& socket() {
			return socket_;
		}

		// Read headers from client and then handle_read
		void start()
		{
/*
			// init interpolator (must be one per session):
			interpU_ = interpolation< vector >::New("cellPoint",*U_ptr_);
*/
			asio::async_read_until(socket_,
				sbuff_,
				'\n',
				[this](const boost::system::error_code& ec, std::size_t bytes_transferred)
				{
					onRequestReceived(ec, bytes_transferred);
				});

		}

	private:
		//-----------------------------------------------------------------------------------------------
		void onRequestReceived(const boost::system::error_code& ec, std::size_t bytes_transferred) {
			if (ec != 0) {
				std::cout << "[Session::start] Error occured! Error code = "
					<< ec.value()
					<< ". Message: " << ec.message();

				return;
			}

			std::cout << "[onRequestReceived] bytes_transferred (inbound): " << bytes_transferred << std::endl;

			// Process the request.
			response_ = processRequest(sbuff_, bytes_transferred);

			std::cout << "[onRequestReceived] response---------------------------------" << std::endl
				<< response_ 
				<<       "-----------------------------------------------------response"
				<< std::endl;

			// Initiate asynchronous write operation.
			asio::async_write(socket_,
				asio::buffer(response_),
				[this](const boost::system::error_code& ec, std::size_t bytes_transferred)
				{
					onResponseSent(ec, bytes_transferred);
				});
		}

		std::string processRequest(asio::streambuf& b, std::size_t bytes_transferred) {
			// In this method we parse the request, process it
			// and prepare the request.

			std::istream is(&b);
    		std::string s;
    		std::getline(is, s);
			std::cout << "[processRequest] request: " << s << std::endl;

			// Prepare and return the response message. 
			std::string response("set fcs/rudder-cmd-norm 1\n");
			response.append("set atmosphere/gust-east-fps 5.0\n");
			response.append("set atmosphere/gust-north-fps 12.0\n");
			response.append("set atmosphere/gust-down-fps 0.0\n");
			
			return response;
		}

		void onResponseSent(const boost::system::error_code& ec, std::size_t bytes_transferred) {
			if (ec != 0) {
				std::cout << "[onResponseSent] Error occured! Error code = "
					<< ec.value()
					<< ". Message: " << ec.message() << std::endl;
			}

			std::cout << "[onResponseSent] ..." << std::endl;
			std::cout << "[onResponseSent] bytes_transferred (outbound): " << bytes_transferred << std::endl;

			asio::async_read_until(socket_,
				sbuff_,
				'\n',
				[this](const boost::system::error_code& ec, std::size_t bytes_transferred)
				{
					onRequestReceived(ec, bytes_transferred);
				});

		}

		//-----------------------------------------------------------------------------------------------
		asio::ip::tcp::socket socket_;
		asio::streambuf sbuff_;
		std::string response_;
/*
		const Foam::fvMesh *mesh_ptr_;
		const volVectorField *U_ptr_;
		autoPtr< interpolation< vector > > interpU_;
*/
};

class Server
{
	public:
		Server(boost::asio::io_service& io_service, short port /*, const Foam::fvMesh *mesh_ptr, const volVectorField *U_ptr */)
		: io_service_(io_service), acceptor_(io_service, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)) // , mesh_ptr_(mesh_ptr), U_ptr_(U_ptr)
		{
			start_accept();
		}			
		//-----------------------------------------------------------------------------------------------
		void start_accept()	{
			Session* new_session = new Session(io_service_ /*, mesh_ptr_, U_ptr_ */ );
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
/*
		const Foam::fvMesh *mesh_ptr_;
		const volVectorField *U_ptr_;
*/
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

/*
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
*/
		//=============================================
		// Server logic

		std::cout << "TCP asynchronous server listening on port "
			<< port_num << std::endl;
		
		boost::asio::io_service io_service;
		Server s(io_service, port_num /*, mesh_ptr, U_ptr */ );
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
