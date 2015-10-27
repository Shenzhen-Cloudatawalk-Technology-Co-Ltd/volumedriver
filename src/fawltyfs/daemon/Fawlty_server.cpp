// Copyright 2015 iNuron NV
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This autogenerated skeleton file illustrates how to build a server.
// You should copy it to another filename to avoid overwriting it.
#include <youtils/Main.h>
#include "FawltyService.h"
#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/server/TSimpleServer.h>
#include <thrift/transport/TServerSocket.h>
#include <thrift/transport/TBufferTransports.h>
#include <boost/program_options.hpp>

namespace
{

using namespace ::apache::thrift;
using namespace ::apache::thrift::protocol;
using namespace ::apache::thrift::transport;
using namespace ::apache::thrift::server;
using namespace fawlty_daemon;

using boost::shared_ptr;
namespace po = boost::program_options;

class FawltyServer : public youtils::MainHelper
{
public:
    FawltyServer(int argc,
                 char** argv)
        : MainHelper(argc, argv)
        , normal_options_("Normal options")
    {
        normal_options_.add_options()
            ("port", po::value<uint16_t>(&port_)->default_value(9090), "port to listen to thrift requests");

    }

    virtual void
    log_extra_help(std::ostream& strm)
    {
        strm << normal_options_;
    }


    void
    parse_command_line_arguments()
    {
        parse_unparsed_options(normal_options_,
                               AllowUnregisteredOptions::T,
                               vm_);
    }

    virtual void
    setup_logging()
    {
        MainHelper::setup_logging();
    }


    virtual int
    run()
    {
        shared_ptr<FawltyService> handler(new FawltyService());
        shared_ptr<TProcessor> processor(new FawltyProcessor(handler));
        shared_ptr<TServerTransport> serverTransport(new TServerSocket(port_));
        shared_ptr<TTransportFactory> transportFactory(new TBufferedTransportFactory());
        shared_ptr<TProtocolFactory> protocolFactory(new TBinaryProtocolFactory());

        TSimpleServer server(processor, serverTransport, transportFactory, protocolFactory);
        handler->set_server(server);
        try
        {
            server.serve();
        }
        catch(apache::thrift::transport::TTransportException& t)
        {
            return 3;
        }
        catch(...)
        {
            return 1;
        }

        return 0;
    }

private:
    uint16_t port_;
    po::options_description normal_options_;

};

}

MAIN(FawltyServer)
