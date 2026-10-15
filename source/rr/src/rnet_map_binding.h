// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "rnet_addon.h"
#include <algorithm>
#include <string>

namespace rnet { namespace content {
// A Welcome names the server's map, not a trusted client filesystem path.
// Only the exact lookup whose bytes were verified may reach the map loader.
class MapBinding {
    std::string announced_, lookup_, verified_;
    static std::string folded(std::string value)
    { for(char& c:value)if(c>='A'&&c<='Z')c+='a'-'A';return value; }
public:
    void clear() {announced_.clear();lookup_.clear();verified_.clear();}
    bool announce(const std::string& welcome,const std::string& offered,std::string& error)
    {
        clear();
        if(!rnetaddon::validPath(offered)||offered.find('/')!=std::string::npos
            ||offered.size()<5||folded(offered.substr(offered.size()-4))!=".map")
        {error="Invalid server map filename";return false;}
        std::string path=welcome;
        std::replace(path.begin(),path.end(),'\\','/');
        size_t const slash=path.find_last_of('/');
        if(folded(slash==std::string::npos?path:path.substr(slash+1))!=folded(offered))
        {error="Server map announcement and content identity differ";return false;}
        announced_=welcome;
        lookup_=rnetaddon::validPath(path)?path:offered;
        error.clear();return true;
    }
    const std::string& lookup() const {return lookup_;}
    void verified(const std::string& path) {verified_=path;}
    const char* resolve(const char* requested) const
    {
        if(!requested||verified_.empty()
            ||(requested!=announced_&&requested!=verified_))return nullptr;
        return verified_.c_str();
    }
};
} }
