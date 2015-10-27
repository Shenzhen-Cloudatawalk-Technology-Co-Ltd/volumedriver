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

#ifndef MAIN_EVENT_H_
#define MAIN_EVENT_H_

#include "Logger.h"

namespace youtils
{

class MainEvent
{
public:
    MainEvent(const std::string& what,
              Logger::logger_type& logger);

    ~MainEvent();

private:
    const std::string what_;
    Logger::logger_type& logger_;
};

#define MAIN_EVENT(arg)                                                 \
    ::std::stringstream m_e_s_s__;                                      \
    m_e_s_s__ << arg;                                                   \
    ::youtils::MainEvent __main_event__(m_e_s_s__.str(), getLogger__());

}

#endif // MAIN_EVENT_H_
