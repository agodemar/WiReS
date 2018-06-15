/*
* BoostServer.cpp
*
*  Created on: 8 Jun 2018
*      Author: Agostino De Marco
*/

#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <boost/asio.hpp>
#include <boost/bind.hpp>

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
using boost::asio::ip::tcp;

//========================
// GLOBALS
//========================

const char*  my_port = "1025";

const int MAX_DATA_LENGTH = 200;

//========================
// FUNCTIONS
//========================

void removeSpaceAndNewline(char* s)
{
	char* s2 = s;
	do {
		if (*s2 != ' ' && *s2 != '\n')
		*s++ = *s2;
	} while (*s2++);
}

//-----------------------------------------------------------------------------------------------

void tokenize_buffer(char* buffer, char symbol, long double* array)
{

	// Print array for debug
	std::cout << "\n\nReceived buffer: " << buffer;

	// removeSpaceAndNewline(buffer);

	std::string bufstr(buffer), substr;

	// Print for debug
	Info << "\n\nRECV: \n" << bufstr << '\n';

	/*

	TODO: parse the buffer received from socket

	for (int i=0; i<3; i++) {

		unsigned int pos = bufstr.find(symbol, 0);
		// if (pos == string::npos) {
		// 	std::cerr << "ERROR on read(): could not tokenize buffer correctly\n";
		// 	perror("");
		// 	exit(1);
		// }

		substr = bufstr.substr(0, pos);
		array[i] = atof(substr.c_str());
		bufstr = bufstr.substr(pos+1, string::npos);	// update bufstr with remaining part of buffer
	}
	*/
	array[0] = 0.0; // 29.593978; // latitude
	array[1] = 0.0; // -95.163839; // longitude
	array[2] = 400.0; // agl-m

	/* Print for debug
	for (int i=0; i<=2; i++) {
		std::cout << "array[" << i << "]=" << std::fixed << std::setprecision(6) << array[i] << ", ";
	}
	std::cout << '\n';
	*/
}

std::string to_string(double dbl)
{
	std::ostringstream strs;
	strs << std::fixed << std::setprecision(6) << dbl;
	std::string str = strs.str();
	return str;
}

//-----------------------------------------------------------------------------------------------

inline double mtoft(double dbl)
{
	return 3.28084*dbl;
}

//-----------------------------------------------------------------------------------------------

//========================
// CLASS: SESSION
//========================

class session
{

	public:

		//-----------------------------------------------------------------------------------------------
		session(
			boost::asio::io_service& io_service,
			const Foam::fvMesh *mesh_ptr,
			const volVectorField *U_ptr)
			: socket_(io_service),
				mesh_ptr_(mesh_ptr),
				U_ptr_(U_ptr) {
		}
		//-----------------------------------------------------------------------------------------------
		tcp::socket& socket() {
			return socket_;
		}
		//-----------------------------------------------------------------------------------------------
		// Read headers from client and then handle_read
		void start() {

			// init interpolator (must be one per session):
			interpU_ = interpolation< vector >::New("cellPoint",*U_ptr_);

			socket_.async_read_some(
				boost::asio::buffer(data_, MAX_DATA_LENGTH),
				boost::bind(
					&session::handle_read,
					this,
					boost::asio::placeholders::error,
					boost::asio::placeholders::bytes_transferred)
				);

			Info << "\n\nSTART: " << data_ << endl;
		}
		//-----------------------------------------------------------------------------------------------

	private:
		//-----------------------------------------------------------------------------------------------
		// Process input, interpolate, process result, write to client and then handle_write
		void handle_read(
			const boost::system::error_code& error,
			size_t bytes_transferred) {

			if (!error) {

				long double position[3] = {0.0, 0.0, 0.0};
				std::string wind_compon	= "";

				// Stage 1: Process buffer (tokenize)
				tokenize_buffer(data_, ',', position);

				// Stage 2: Prepare for interpolation
				// Server is completely asynchronous and deals with each client separately
				// Therefore, only one point per session per reading is needed
				pointField pts(1);

				double lat = position[0], // expected to be in degrees
					lon = position[1],    // expected to be in degrees
					alt = position[2];    // expected to be in meters
				int zone;
				bool northp;
				double x, y, gamma, k;

				// Convert from Lat-Lon to UTM easting (x) and northing (y)
				// https://geographiclib.sourceforge.io/html/classGeographicLib_1_1UTMUPS.html
				GeographicLib::UTMUPS::Forward(
					lat, lon, // [in] geographic coordinates of point, (deg)
					zone, // [out] https://upload.wikimedia.org/wikipedia/commons/e/ed/Utm-zones.jpg
					northp, // [out] hemisphere (1 means north, 0 means south). 
					x, // [out] easting of point (m).  
					y, // [out] northing of point (m)
					gamma, // [out] meridian convergence at point (degrees)
					k); // [out] scale of projection at point

				pts[0] = point(x, y, alt);

				Info
					<< "Lat=" << lat << " and Lon=" << lon
					<< " converted to " << "x=" << x << " and " << "y=" << y << " UTM" << endl;

				Info << nl
					<< "zone=" << zone << ", northp=" << northp << ", gamma=" << gamma
					<< ", k=" << k << endl;

				// Stage 3: Interpolate
				Info << "Interpolating U" << endl;
				labelList pcells(pts.size());
				vectorField resultU(pts.size());
				forAll(pts,i){

					pcells[i]  = mesh_ptr_->findCell(pts[i]);

					if (pcells[i] == -1) {
						// point is not in the grid
						Info << i << ": p = " << pts[i] << " is out of grid" << endl;
						wind_compon = "0.0,0.0,0.0";
					}
					else {
						resultU[i] = interpU_->interpolate(pts[i], pcells[i]);

						Info << i << ": p = " << pts[i] << ", U = " << resultU[i] << endl;

						// Stage 4: Process data
						wind_compon += 
							to_string(mtoft(   resultU[i][1])) + "," +  // North component is the second
							to_string(mtoft(   resultU[i][0])) + "," + // East component is the first
							to_string(mtoft(-1*resultU[i][2])); // Down component is the opposite of third
					}

					// Print for debug
					Info << "wind_compon string: " << wind_compon << endl;
				}
				Info << "End of interpolation" << endl;

				// Write
				// Print for debug
				Info << "SEND: " << wind_compon.c_str() << endl;

				// TODO: write back to socket: set <path-to-property> value

				boost::asio::async_write(
					socket_,
					boost::asio::buffer(wind_compon.c_str(), bytes_transferred),
					boost::bind(&session::handle_write,
					this,
					boost::asio::placeholders::error)
				);
			}
			else {
				delete this;
			}
		}
		//-----------------------------------------------------------------------------------------------
		// Start reading again and then handle_read --> loop until End Of Data
		void handle_write(const boost::system::error_code& error) {
			if (!error) {
				socket_.async_read_some(
					boost::asio::buffer(data_, MAX_DATA_LENGTH),
					boost::bind(&session::handle_read,
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
		tcp::socket socket_;
		//enum { MAX_DATA_LENGTH = 200 };
		char data_[MAX_DATA_LENGTH];
		const Foam::fvMesh *mesh_ptr_;
		const volVectorField *U_ptr_;
		autoPtr< interpolation< vector > > interpU_;
};

//-----------------------------------------------------------------------------------------------

//========================
// CLASS: SERVER
//========================

class server
{

	public:
		//-----------------------------------------------------------------------------------------------
		server(
			boost::asio::io_service& io_service,
			short port,
			const Foam::fvMesh *mesh_ptr,
			const volVectorField *U_ptr)
			: io_service_(io_service),
				acceptor_(io_service, tcp::endpoint(tcp::v4(), port)),
				mesh_ptr_(mesh_ptr),
				U_ptr_(U_ptr)
			{
				start_accept();
			}

	private:
		//-----------------------------------------------------------------------------------------------
		void start_accept()	{
			session* new_session = new session(io_service_, mesh_ptr_, U_ptr_);
			acceptor_.async_accept(
				new_session->socket(),
				boost::bind(
					&server::handle_accept,
					this,
					new_session,
					boost::asio::placeholders::error)
				);
		}
		//-----------------------------------------------------------------------------------------------
		void handle_accept(
			session* new_session,
			const boost::system::error_code& error) {

			if (!error)
				new_session->start();
			else
				delete new_session;

			start_accept();
		}
		//-----------------------------------------------------------------------------------------------
		boost::asio::io_service& io_service_;
		tcp::acceptor acceptor_;
		const Foam::fvMesh *mesh_ptr_;
		const volVectorField *U_ptr_;
};

//-----------------------------------------------------------------------------------------------

//========================
// MAIN
//========================

int main(int argc, char* argv[]) {

	try {

		/* Print argument list
		Foam::Info << "argc = " << argc << Foam::endl;
		for(int i=0; i< argc; i++){
			Foam::Info << argv[i] << ", ";
		}
		Foam::Info << Foam::endl;
		*/

		//-----------------------------------------------------------------------------------------------
		// OPTIONAL ARGUMENTS PROCESSING
		//-----------------------------------------------------------------------------------------------

		// Assign port based on argument 1 (whether or not it's present)
		if(argc == 2) {
				my_port = argv[1];

				// Remove argument 1 and pass all other arguments to OF
				for(int i=1; i <= argc-1; i++) {
					argv[i] = argv[i+1];
				}
				argc--;
		}


		// #include "createMesh.H"

		// setRootCase.H
    	Foam::argList args(argc, argv);
		if (!args.checkRootCase()) {
			Foam::FatalError.exit();
		}

		//-----------------------------------------------------------------------------------------------
		// LOADING FIELDS AND INITIALIZATION
		//-----------------------------------------------------------------------------------------------

		Foam::Info << "Creating server on port " << my_port << Foam::endl;

		// createTime.H
		Foam::Info << "Create time\n" << Foam::endl;
		//read information from system/controlDict: mind for "startFrom latestTime;" entry
		Foam::Time runTime(Foam::Time::controlDictName, args);

		// createMesh.H
		Foam::Info << "\nCreate mesh for time = " << runTime.timeName() << Foam::nl << Foam::endl;

		Foam::fvMesh mesh(
			Foam::IOobject(
				Foam::fvMesh::defaultRegion,
				runTime.timeName(),
				runTime,
				Foam::IOobject::MUST_READ)
			);

		// load field U:
		Info << "Reading field U\n" << endl;
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

		// init server: wait for clients
		Info << "Server is ready: listening on port " << my_port << endl;
		boost::asio::io_service io_service;
		server s(io_service, atoi(my_port), mesh_ptr, U_ptr);
		io_service.run();
	}
	catch (std::exception& e) {
		std::cerr << "Exception: " << e.what() << "\n";
	}

	return 0;
}
