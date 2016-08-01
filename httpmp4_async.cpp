#include <iostream>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>

#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/thread.hpp>
#include <boost/thread/scoped_thread.hpp>
#include <boost/chrono.hpp>
#include <boost/regex.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>


using boost::asio::ip::tcp;
using namespace boost::posix_time;

boost::thread *t;

std::string dateTime()
{
    ptime now(microsec_clock::local_time());

    std::string dt = to_iso_string(now);
    std::stringstream ret;
    ret << dt.substr(0,4) << "-" 
        << dt.substr(4,2) << "-"
        << dt.substr(6,2) << " "
        << dt.substr(9,2) << ":"
        << dt.substr(11,2) << ":"
        << dt.substr(13,2) << "."
        << dt.substr(16,3) << " ";
    //std::cout << ret.str() << std::endl;

    //static std::locale loc(std::wcout.getloc(), new wtime_facet(L"%Y-%m-%d %H:%M:%S.%f"));
    //std::basic_stringstream<wchar_t> wss;
    //wss.imbue(loc);
    //wss << now;
    //std::wcout << wss.str() << std::endl;

    return ret.str();

}

void wait(int seconds)
{
    boost::this_thread::sleep_for(boost::chrono::seconds{seconds});
}

class client
{
    public:
        client(boost::asio::io_service& io_service,
                const std::string& inputurl)
            : resolver_(io_service),
            socket_(io_service)
    {
        std::string protocol, server, path;

        // parse URL
        url = inputurl;
        boost::regex ex("(http|https)://([^/ :]+):?([^/ ]*)(/?[^ #?]*)\\x3f?([^ #]*)#?([^ ]*)");
        boost::cmatch what;
        if(regex_match(url.c_str(), what, ex)) 
        {
            protocol = std::string(what[1].first, what[1].second);
            server = std::string(what[2].first, what[2].second);
            path = std::string(what[4].first, what[4].second);
        } else {
            std::cout << dateTime() << "Error: URL format error." << std::endl;
            return;
        }

        // Specify "Connection: close" in request header so that the server will 
        // close the socket after transmitting the response. This will
        // allow us to treat all data up until the EOF as the content.
        std::ostream request_stream(&request_);
        request_stream << "GET " << path << " HTTP/1.0\r\n";
        request_stream << "Host: " << server << "\r\n";
        request_stream << "Accept: */*\r\n";
        request_stream << "Connection: close\r\n\r\n";

        // Start an asynchronous resolve to translate the server and service names
        // into a list of endpoints.
        tcp::resolver::query query(server, protocol.c_str());
        resolver_.async_resolve(query,
                boost::bind(&client::handle_resolve, this,
                    boost::asio::placeholders::error,
                    boost::asio::placeholders::iterator));
    }

        bool getResponseDone() { return responseDone; }
        void setResponseDone(bool flag) 
        { 
            mtx.lock();
            responseDone=flag; 
            mtx.unlock();
        }

    private:

        void thread_handle_response_body()
        {
            unsigned char boxsizeStr[4];
            char boxtypeStr[5];     
            unsigned long boxsize; 

            while(1) {
                mtx.lock();
                if ( response_.size() < 8 ) {
                    mtx.unlock();
                    wait(1);
                    if ( getResponseDone() ) {
                        break;
                    } else {
                        continue;
                    }
                } else {
                    // 1. box size and type
                    //std::cout << "response size = " << response_.size() << std::endl;
                    response_.sgetn( (char*)boxsizeStr, 4);
                    response_.sgetn( (char*)boxtypeStr, 4);
                    boxtypeStr[4] = '\000';
                    boxsize = ((unsigned int)boxsizeStr[0]<<24) + 
                        ((unsigned int)boxsizeStr[1]<<16) + 
                        ((unsigned int)boxsizeStr[2]<<8) + 
                        (unsigned int)boxsizeStr[3];
                    //std::cout << "box size = " << boxsize << " type = " << boxtypeStr << std::endl;
                    //std::cout << "response size (after box) = " << response_.size() << std::endl;

                    if ( !strcmp(boxtypeStr, "moof") || 
                            !strcmp(boxtypeStr, "traf") ) {
                        std::cout << dateTime() << "Found box of type " << boxtypeStr 
                            << " and size " << boxsize << std::endl;
                        mtx.unlock();
                        continue;
                    }

                    if ( !strcmp(boxtypeStr, "mfhd") ||
                            !strcmp(boxtypeStr, "tfhd") ||
                            !strcmp(boxtypeStr, "trun") ||
                            !strcmp(boxtypeStr, "uuid") ||
                            !strcmp(boxtypeStr, "mdat") ) {
                        std::cout << dateTime() << "Found box of type " << boxtypeStr 
                            << " and size " << boxsize << std::endl;
                    }

                    char * boxBodyBuffer;
                    long boxBodySize = boxsize-8;
                    long availableSize = response_.size();
                    long consumedSize = 0;

                    if ( !strcmp(boxtypeStr, "mdat") ) {
                        std::cout << dateTime() << "Content of mdat box is: ";
                    }

                    // 2. consume the box body content until it's finished
                    while ( consumedSize+availableSize < boxBodySize ) {
                        boxBodyBuffer = new char [availableSize+1];
                        if ( boxBodyBuffer == NULL ) {
                            printf("Error: memory allocation failed (%s, %d)", 
                                    __FILE__, __LINE__);
                            // throw an error and exit
                            mtx.unlock();
                            return;
                        }
                        response_.sgetn( boxBodyBuffer, availableSize );
                        boxBodyBuffer[availableSize] = '\000';
                        if ( !strcmp(boxtypeStr, "mdat") ) {
                            std::cout << boxBodyBuffer << std::endl;
                        }
                        consumedSize += availableSize;
                        delete [] boxBodyBuffer;
                        mtx.unlock();
                        wait(1);
                        mtx.lock();
                        availableSize = response_.size();
                    }

                    if ( consumedSize+availableSize >= boxBodySize ) {
                        boxBodyBuffer = new char [boxBodySize-consumedSize+1];
                        if ( boxBodyBuffer == NULL ) {
                            printf("Error: memory allocation failed (%s, %d)", 
                                    __FILE__, __LINE__);
                            // throw an error and exit
                            mtx.unlock();
                            return;
                        }
                        response_.sgetn( boxBodyBuffer, boxBodySize-consumedSize );
                        boxBodyBuffer[boxBodySize-consumedSize] = '\000';
                        if ( !strcmp(boxtypeStr, "mdat") ) {
                            std::cout << boxBodyBuffer << std::endl;
                        }
                        delete [] boxBodyBuffer;
                    }
                    // We have finished handling the box body content here.
                }
                mtx.unlock();
            }
        }

        void handle_resolve(const boost::system::error_code& err,
                tcp::resolver::iterator endpoint_iterator)
        {
            if (!err)
            {
                // Attempt a connection to each endpoint in the list until we
                // successfully establish a connection.
                boost::asio::async_connect(socket_, endpoint_iterator,
                        boost::bind(&client::handle_connect, this,
                            boost::asio::placeholders::error));
            }
            else
            {
                std::cout << dateTime() << "Error: " << err.message() << "\n";
            }
        }

        void handle_connect(const boost::system::error_code& err)
        {
            if (!err)
            {
                // The connection was successful. Send the request.
                boost::asio::async_write(socket_, request_,
                        boost::bind(&client::handle_write_request, this,
                            boost::asio::placeholders::error));
            }
            else
            {
                std::cout  << dateTime() << "Error: " << err.message() << "\n";
            }
        }

        void handle_write_request(const boost::system::error_code& err)
        {
            if (!err)
            {
                // Read the response status line. The response_ streambuf will
                // automatically grow to accommodate the entire line. The growth may be
                // limited by passing a maximum size to the streambuf constructor.
                boost::asio::async_read_until(socket_, response_, "\r\n",
                        boost::bind(&client::handle_read_status_line, this,
                            boost::asio::placeholders::error));
            }
            else
            {
                std::cout << dateTime() << "Error: " << err.message() << "\n";
            }
        }

        void handle_read_status_line(const boost::system::error_code& err)
        {
            if (!err)
            {
                // Check that response is OK.
                std::istream response_stream(&response_);
                std::string http_version;
                response_stream >> http_version;
                unsigned int status_code;
                response_stream >> status_code;
                std::string status_message;
                std::getline(response_stream, status_message);
                if (!response_stream || http_version.substr(0, 5) != "HTTP/")
                {
                    std::cout << dateTime() << "Invalid response\n";
                    return;
                }
                if (status_code != 200)
                {
                    std::cout << dateTime() << "Response returned with status code ";
                    std::cout << status_code << "\n";
                    return;
                } else {
                    std::cout << dateTime() << "Successfully loaded file " << url << std::endl;
                }

                // Read the response headers, which are terminated by a blank line.
                boost::asio::async_read_until(socket_, response_, "\r\n\r\n",
                        boost::bind(&client::handle_read_headers, this,
                            boost::asio::placeholders::error));
            }
            else
            {
                std::cout << dateTime() << "Error: " << err << "\n";
            }
        }

        void handle_read_headers(const boost::system::error_code& err)
        {
            if (!err)
            {
                // Process the response headers.
                std::istream response_stream(&response_);
                std::string header;
                while (std::getline(response_stream, header) && header != "\r") {
                    // let header pass
                    // std::cout << header << "\n";
                }
                // std::cout << "\n";

                // RESPONSE BODY
#if 0
                if (response_.size() > 0) {
                    std::cout << &response_;
                }
#endif

                // We start another thread to consume the response body from here
                t = new boost::thread {&client::thread_handle_response_body, this};

                // Start reading remaining data until EOF.
                mtx.lock();
                boost::asio::async_read(socket_, response_,
                        boost::asio::transfer_at_least(1),
                        boost::bind(&client::handle_read_content, this,
                            boost::asio::placeholders::error));
                mtx.unlock();
            }
            else
            {
                std::cout << dateTime() << "Error: " << err << "\n";
            }
        }

        void handle_read_content(const boost::system::error_code& err)
        {
            mtx.lock();
            if (!err)
            {
#if 0
                if (response_.size() > 0) {
                    std::cout << &response_;
                }
#endif
                // Continue reading remaining data until EOF.
                boost::asio::async_read(socket_, response_,
                        boost::asio::transfer_at_least(1),
                        boost::bind(&client::handle_read_content, this,
                            boost::asio::placeholders::error));
            }
            else if (err != boost::asio::error::eof)
            {
                std::cout << dateTime() << "Error: " << err << "\n";
            }
            mtx.unlock();
        }

        std::string url;
        tcp::resolver resolver_;
        tcp::socket socket_;
        boost::asio::streambuf request_;
        boost::asio::streambuf response_;
        bool responseDone=false;
        std::mutex mtx;
};


int main(int argc, char* argv[])
{
    if (argc != 2)
    {
        std::cout << "Usage: " << argv[0] << " <URL>" << std::endl;
        std::cout << "Example:" << std::endl;
        std::cout << "  " << argv[0] << " http://demo.castlabs.com/tmp/text.mp4" << std::endl;
        return 1;
    }

    try
    {
        boost::asio::io_service io_service;
        client c(io_service, argv[1]);
        io_service.run();

        c.setResponseDone(true);

        t->join();
        delete t;
    }
    catch (std::exception& e)
    {
        std::cout << dateTime() << "Exception: " << e.what() << "\n";
    }


    return 0;
}

