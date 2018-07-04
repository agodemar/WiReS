#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <iomanip>
#include <cstdlib>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
#include <boost/make_shared.hpp>
#include <boost/spirit/include/qi.hpp>
#include <boost/spirit/include/phoenix_core.hpp>
#include <boost/spirit/include/phoenix_operator.hpp>
#include <boost/spirit/include/phoenix_stl.hpp>
#include <boost/algorithm/string/join.hpp>
#include <boost/range/adaptor/transformed.hpp>

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

namespace wires
{
    namespace qi = boost::spirit::qi;
    namespace ascii = boost::spirit::ascii;
    namespace phoenix = boost::phoenix;

    //  number list compiler
    template <typename Iterator>
    bool parse_numbers(Iterator first, Iterator last, std::vector<double>& v)
    {
        using qi::double_;
        using qi::phrase_parse;
        using qi::_1;
        using ascii::space;
        using phoenix::push_back;

        bool r = phrase_parse(first, last,

            //  Begin grammar
            (
                double_[push_back(phoenix::ref(v), _1)]
                    >> *(',' >> double_[push_back(phoenix::ref(v), _1)])
            )
            ,
            //  End grammar

            space);

        if (first != last) // fail if we did not get a full match
            return false;
        return r;
    }
	std::string to_string(double dbl) {
		std::ostringstream strs;
		strs << std::fixed << std::setprecision(6) << dbl;
		std::string str = strs.str();
		return str;
	}
	inline double mtoft (double dbl) {
		return 3.28084*dbl;
	}
}

//===============================================
// GLOBALS


//===============================================
// Server logic

class Session
{
	public:
		Session(boost::asio::io_service& io_service, const fvMesh *mesh_ptr, const volVectorField *U_ptr)
			: socket_(io_service), out_socket_(io_service), mesh_ptr_(mesh_ptr), U_ptr_(U_ptr)
		{
		}

		asio::ip::tcp::socket& socket() {
			return socket_;
		}

		// Read headers from client and then handle_read
		void start()
		{
			// init interpolator (must be one per session):
			interpU_ = interpolation< vector >::New("cellPoint",*U_ptr_);

			//===============================================================
			// local socket, for outbound data to JSBSim

			// get the address of the client
			client_address_ = socket_.remote_endpoint().address().to_string();
			std::cout << "[Session::start] client address: " << client_address_ << std::endl;
			// Construct endpoint
			out_endpoint_ = boost::make_shared<boost::asio::ip::tcp::endpoint>(
				socket_.remote_endpoint().address(), 
				1139 // <=============================
				);

			std::cout << "[Session::start] End point for outbound data declared at "
				<< out_endpoint_->address() << " on port " << out_endpoint_->port() << std::endl;

			// open the local socket
			boost::system::error_code ec;
			out_socket_.open(out_endpoint_->protocol(), ec);
			if (ec.value() != 0) {
				// Failed to open the socket.
				std::cout << "[Session::start] Failed to open the socket! Error code = " << ec.value() 
					<< ". Message: " << ec.message() << std::endl;
			}
			std::cout << "[Session::start] Socket for outbound data opened." << std::endl;

			// connect
			try {
				out_socket_.connect(*out_endpoint_);
				std::cout << "[Session::start] Socket for outbound data connected." << std::endl;
			}
			catch (system::system_error &e) {
				std::cout << "[Session::start] Error occurred connecting to output socket! Error code = " << e.code()
					<< ". Message: " << e.what() << std::endl;
					exit(1);
			}

			//===============================================================

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
				std::cout << "[Session::onRequestReceived] Error occurred! Error code = "
					<< ec.value()
					<< ". Message: " << ec.message();

				return;
			}

			std::cout << "[onRequestReceived] bytes_transferred (inbound): " << bytes_transferred << std::endl;

			// Process the request.
			response_ = processRequest(sbuff_, bytes_transferred);

			std::cout << "[Session::onRequestReceived] response---------------------------------" << std::endl
				<< response_ 
				<<       "-----------------------------------------------------response"
				<< std::endl;

			// Initiate synchronous write operation.
			boost::system::error_code ignored_error;
			asio::write(out_socket_, // <==========================================================
				asio::buffer(response_),
				asio::transfer_all(), 
				ignored_error);
		}

		std::string processRequest(asio::streambuf& b, std::size_t bytes_transferred) {
			// In this method we parse the request, process it
			// and prepare the request.

			std::istream is(&b);
    		std::string s;
    		std::getline(is, s);
			std::cout << "[Session::processRequest] request: " << s << std::endl;

			// Parse request
			// Expected: lat (deg), lon (deg), h (m)
			std::vector<double> v;
			if (wires::parse_numbers(s.begin(), s.end(), v))
			{
				std::cout << "-------------------------\n";
				std::cout << "Parsing succeeded\n";
				// vector v contains double-s
				// Expected: Time = v[0]; lat = v[1]; lon = v[2]; height = v[3]

				std::cout 
      				<< algorithm::join( v | 
               			adaptors::transformed( static_cast<std::string(*)(double)>(std::to_string) ), 
               			", " );

				std::cout << "\n-------------------------\n";

				// Data check
				if (v.size() >= 4)
				{
					// data arranged as expected, process them

					std::string wind_compon	= "";

					// Server is completely asynchronous and deals with each client separately
					// Therefore, only one point per session per reading is needed
					Foam::pointField pts(1);

					double 	
						lat = v[1], // expected to be in degrees
						lon = v[2], // expected to be in degrees
						alt = v[3]; // expected to be in meters
					int zone;
					bool northp;
					double x, y, gamma, k;

					// Convert from Lat-Lon to UTM easting (x)  and northing (y)
					GeographicLib::UTMUPS::Forward(lat, lon, zone, northp, x, y, gamma, k);
					pts[0] = point(x, y, alt);

					std::cout << "Lat=" << lat << " and Lon=" << lon
						<< " converted to " << "x=" << x << " and " << "y=" << y << " UTM" << std::endl;

					// Interpolate
					std::cout << "Interpolating U" << std::endl;

					Foam::labelList pcells(pts.size());
					Foam::vectorField resultU(pts.size());
					forAll(pts, i){

						pcells[i]  = mesh_ptr_->findCell(pts[i]);

						if (pcells[i] == -1) {
							// point is not in the grid
							Foam::Info << i << ": p = " << pts[i] << " is out of grid" << Foam::endl;
							wind_compon = "0.0, 0.0, 0.0";
						}
						else {
							// point is in the grid, interpolate
							resultU[i] = interpU_->interpolate(pts[i],pcells[i]);

							Foam::Info << i << ": p = " << pts[i] << ", U = " << resultU[i] << Foam::endl;

							// Process data
							wind_compon += 
								wires::to_string(wires::mtoft(resultU[i][1])) // North component is the second
								 + "," +
								wires::to_string(wires::mtoft(resultU[i][0])) // East component is the first
								 + "," +
								wires::to_string(wires::mtoft(-1*resultU[i][2])); // Down component is the opposite of third
						}
						// Print for debug
						std::cout << "wind_compon string: " << wind_compon << std::endl;
					}
					std::cout << "End of interpolation" << std::endl;
				}
				else {
					// TODO: do nothing?
				}
			}
			else
			{
				std::cout << "-------------------------\n";
				std::cout << "Parsing failed\n";
				std::cout << "-------------------------\n";
				// TODO: do nothing?
			}

			// Prepare and return the response message. 

			// TODO
			std::string response("set fcs/rudder-cmd-norm 1\n");
			response.append("set atmosphere/gust-east-fps 5.0\n");
			response.append("set atmosphere/gust-north-fps 12.0\n");
			response.append("set atmosphere/gust-down-fps 0.0\n");
			
			return response;
		}

		void onResponseSent(const boost::system::error_code& ec, std::size_t bytes_transferred) {
			if (ec != 0) {
				std::cout << "[Session::onResponseSent] Error occurred! Error code = "
					<< ec.value()
					<< ". Message: " << ec.message() << std::endl;
			}

			std::cout << "[Session::onResponseSent] ..." << std::endl;
			std::cout << "[Session::onResponseSent] bytes_transferred (outbound): " << bytes_transferred << std::endl;

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
		std::string client_address_;
		boost::shared_ptr<boost::asio::ip::tcp::endpoint> out_endpoint_;
		asio::ip::tcp::socket out_socket_;
		const fvMesh *mesh_ptr_;
		const volVectorField *U_ptr_;
		autoPtr< interpolation< vector > > interpU_;
};

class Server
{
	public:
		Server(boost::asio::io_service& io_service, short port, const fvMesh *mesh_ptr, const volVectorField *U_ptr)
		: io_service_(io_service), acceptor_(io_service, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)), mesh_ptr_(mesh_ptr), U_ptr_(U_ptr)
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

		const fvMesh *mesh_ptr_;
		const volVectorField *U_ptr_;
};

//===============================================
// Server launcher

int main(int argc, char* argv[])
{
	//=============================================
	// command line options

	std::string app_name = boost::filesystem::basename(argv[0]);
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
		if (vm.count("port")) {
			std::cout << "Port number set to: " << vm["port"].as<unsigned short>() << '\n';
		}
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
		std::cout << "Error occurred! Error code = "
			<< e.code() << ". Message: "
			<< e.what() << std::endl;
	}
	catch (std::exception& e) {
		std::cerr << "Exception: " << e.what() << "\n";
	}

	return 0;
}
