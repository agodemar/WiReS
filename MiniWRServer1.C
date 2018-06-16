/*
* Asynchronous Server application based on boost::asio
* 
* > dpkg -s libboost-dev | grep 'Version'
* Version: 1.58.0.1ubuntu1

* https://theboostcpplibraries.com/boost.asio-network-programming

*
*  Created on: 8 Jun 2018
*      Author: Agostino De Marco
*/
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>
#include <algorithm> 
#include <cctype>
#include <locale>
#include <boost/asio.hpp>
#include <boost/algorithm/string.hpp>

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

//========================================================================================================================
// GLOBALS
//========================================================================================================================

const char*  the_port = "1025";

//========================================================================================================================
// Utility functions
//========================================================================================================================

//========================================================================================================================
// Networking classes
//========================================================================================================================

// Class to manage the memory to be used for handler-based custom allocation.
// It contains a single block of memory which may be returned for allocation
// requests. If the memory is in use when an allocation request is made, the
// allocator delegates allocation to the global heap.
class handler_allocator
{
public:
	handler_allocator()
		: in_use_(false)
	{
	}

	handler_allocator(const handler_allocator&) = delete;
	handler_allocator& operator=(const handler_allocator&) = delete;

	void* allocate(std::size_t size)
	{
		if (!in_use_ && size < sizeof(storage_))
		{
			in_use_ = true;
			return &storage_;
		}
		else
		{
			return ::operator new(size);
		}
	}

	void deallocate(void* pointer)
	{
		if (pointer == &storage_)
		{
			in_use_ = false;
		}
		else
		{
			::operator delete(pointer);
		}
	}

private:
	// Storage space used for handler-based custom memory allocation.
	typename std::aligned_storage<1024>::type storage_;

	// Whether the handler-based custom allocation storage has been used.
	bool in_use_;
};

// Wrapper class template for handler objects to allow handler memory
// allocation to be customised. Calls to operator() are forwarded to the
// encapsulated handler.
template <typename Handler>
class custom_alloc_handler
{
public:
	custom_alloc_handler(handler_allocator& a, Handler h)
		: allocator_(a), handler_(h)
	{
	}

	template <typename ...Args>
	void operator()(Args&&... args)
	{
		handler_(std::forward<Args>(args)...);
	}

	friend void* asio_handler_allocate(std::size_t size,
		custom_alloc_handler<Handler>* this_handler)
	{
		return this_handler->allocator_.allocate(size);
	}

	friend void asio_handler_deallocate(void* pointer, std::size_t /*size*/,
		custom_alloc_handler<Handler>* this_handler)
	{
		this_handler->allocator_.deallocate(pointer);
	}

private:
	handler_allocator& allocator_;
	Handler handler_;
};

// Helper function to wrap a handler object to add custom allocation.
template <typename Handler>
inline custom_alloc_handler<Handler> make_custom_alloc_handler(
	handler_allocator& a, Handler h)
	{
		return custom_alloc_handler<Handler>(a, h);
	}

class session
	: public std::enable_shared_from_this<session>
{
public:
	session(tcp::socket socket)
		: socket_(std::move(socket))
	{
		count_ = 0;
	}

	void start()
	{
		std::cout << "start__________________" << std::endl;
		do_read();
	}

private:
	void do_read()
	{
		std::cout << "count: " << ++count_ << std::endl;

		auto self(shared_from_this());
		socket_.async_read_some(
			boost::asio::buffer(data_),
			make_custom_alloc_handler(allocator_,
				[this, self](boost::system::error_code ec, std::size_t length)
				{
					if (!ec)
					{
						std::cout << "do_read__________________" << std::endl;
						// print received data
						std::cout << "length=" << length << std::endl;
						std::string msg(data_.begin(), data_.end());
						std::cout << msg << std::endl;
						std::cout << "trimmed_data__________________" << std::endl;
						//trim(msg); // in-place trim
						boost::trim_left(msg);
						boost::trim_right(msg);
						boost::trim_right_if(msg, boost::is_any_of("\n"));
						std::cout << "***" << msg << "***" << std::endl;

						do_write(length);
					}
					else
						std::cout << "ec=" << ec << std::endl;
				})
			);
	}

	void do_write(std::size_t length)
	{
		auto self(shared_from_this());
		boost::asio::async_write(socket_, boost::asio::buffer(data_, length),
			make_custom_alloc_handler(
				allocator_,
				[this, self](boost::system::error_code ec, std::size_t /*length*/)
				{
					if (!ec)
					{
						std::cout << "do_write__________________" << std::endl;
						do_read();
					}
					else
						std::cout << "ec2=" << ec << std::endl;
				})
			);
	}

	// The socket used to communicate with the client.
	tcp::socket socket_;

	// Buffer used to store data received from the client.
	std::array<char, 1024> data_;

	// The allocator to use for handler-based custom memory allocation.
	handler_allocator allocator_;

	int count_;
};

class server
{
public:
	server(boost::asio::io_service& io_service, short port)
		: acceptor_(io_service, tcp::endpoint(tcp::v4(), port)),
		socket_(io_service)
		{
			do_accept();
		}

private:
	void do_accept()
	{
		acceptor_.async_accept(socket_,
			[this](boost::system::error_code ec)
			{
				if (!ec)
				{
					std::make_shared<session>(std::move(socket_))->start();
				}

				do_accept();
			});
	}

	tcp::acceptor acceptor_;
	tcp::socket socket_;
};

int main(int argc, char* argv[])
{
	try
	{
		if (argc != 2)
		{
		std::cerr << "Usage: server <port>\n";
		return 1;
		}

		the_port = argv[1];

		// init server: wait for clients
		Info << "Server is ready: listening on port " << the_port << endl;
		boost::asio::io_service io_service;
		server s(io_service, std::atoi(the_port)); // TODO: pass mesh_ptr, U_ptr to server constructor
		io_service.run();
	}
	catch (std::exception& e)
	{
		std::cerr << "Exception: " << e.what() << "\n";
	}

	return 0;
}