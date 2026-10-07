// FilmPrintLUT 1.1.1: read-only .cube loader. No frame pixels are processed here.
#define NOMINMAX
#include <windows.h>
#include "module2.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using RGB = std::array<double,3>;
struct Entry {
    std::vector<unsigned char> rgba;
    std::array<double,13> metadata{}; // N, domain min/max, output min/span
    int width=0, height=0;
    int refresh=0;
    uint64_t size=0, stamp=0, used=0;
    std::string error;
};
std::mutex mutex;
std::map<std::pair<std::wstring,int>,std::shared_ptr<Entry>> cache;
uint64_t sequence=0;
// Keep returned data alive through the caller's upload, even if another thread evicts it.
thread_local std::shared_ptr<Entry> returned;
constexpr uint64_t max_file_bytes=128ull*1024*1024;

std::wstring wide(const char* s) {
    if (!s || !*s) throw std::runtime_error("Select an external .cube file.");
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,nullptr,0);
    if (!n) throw std::runtime_error("Invalid UTF-8 file path.");
    std::wstring w(n,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s,-1,w.data(),n);
    w.pop_back();
    // Resolve before caching so a changed current directory cannot reuse another file.
    DWORD length=GetFullPathNameW(w.c_str(),0,nullptr,nullptr);
    if (!length) throw std::runtime_error("Invalid file path.");
    std::wstring full(length,L'\0');
    if (!GetFullPathNameW(w.c_str(),length,full.data(),nullptr)) throw std::runtime_error("Invalid file path.");
    full.resize(length-1);
    return full;
}

void end_line(std::istringstream& line) {
    std::string extra;
    if (line>>extra) throw std::runtime_error("Unexpected data after directive/row.");
}
RGB vector3(std::istringstream& line) {
    RGB v;
    if (!(line>>v[0]>>v[1]>>v[2])) throw std::runtime_error("Expected three finite numbers.");
    end_line(line);
    for (double x:v) if (!std::isfinite(x)) throw std::runtime_error("Nonfinite number.");
    return v;
}

void parse(const std::string& text, Entry& entry) {
    std::istringstream lines(text);
    lines.imbue(std::locale::classic());
    std::string raw;
    int n=0, number=0;
    RGB dmin{0,0,0},dmax{1,1,1};
    std::map<std::string,bool> seen;
    std::vector<RGB> rows;
    while (std::getline(lines,raw)) {
        ++number;
        try {
            if (number==1 && raw.compare(0,3,"\xef\xbb\xbf")==0) raw.erase(0,3);
            if (raw.size()>16384) throw std::runtime_error("Line exceeds 16 KiB.");
            bool quoted=false;
            for (size_t i=0;i<raw.size();++i) {
                if (raw[i]=='\"') quoted=!quoted;
                if (raw[i]=='#' && !quoted) {raw.resize(i);break;}
            }
            std::istringstream line(raw); line.imbue(std::locale::classic());
            std::string key;
            if (!(line>>key)) continue;
            if (key=="TITLE" || key=="LUT_3D_SIZE" || key=="DOMAIN_MIN" || key=="DOMAIN_MAX" || key=="LUT_3D_INPUT_RANGE") {
                if (!rows.empty() || seen[key]) throw std::runtime_error("Duplicate or late directive.");
                seen[key]=true;
                if (key=="TITLE") {
                    line>>std::ws;
                    std::string title;
                    std::getline(line,title);
                    // Sony LookProfile files use unquoted descriptive titles.
                    // TITLE is metadata only; never reinterpret it as LUT values.
                    if (title.find_first_not_of(" \t\r")==std::string::npos)
                        throw std::runtime_error("Empty TITLE.");
                    if (title.front()=='\"') {
                        auto closing=title.find('\"',1);
                        if (closing==std::string::npos || title.find_first_not_of(" \t\r",closing+1)!=std::string::npos)
                            throw std::runtime_error("Invalid TITLE.");
                    } else if (title.find('\"')!=std::string::npos) {
                        throw std::runtime_error("Invalid quote in TITLE.");
                    }
                } else if (key=="LUT_3D_SIZE") {
                    if (!(line>>n) || n<2 || n>90) throw std::runtime_error("LUT_3D_SIZE must be 2..90 (D3D11 atlas limit).");
                    end_line(line); rows.reserve(size_t(n)*n*n);
                } else if (key=="LUT_3D_INPUT_RANGE") {
                    if (seen["DOMAIN_MIN"] || seen["DOMAIN_MAX"]) throw std::runtime_error("Conflicting domain declarations.");
                    double low,high;
                    if (!(line>>low>>high) || !std::isfinite(low) || !std::isfinite(high)) throw std::runtime_error("Invalid input range.");
                    end_line(line); dmin.fill(low);dmax.fill(high);
                } else {
                    if (seen["LUT_3D_INPUT_RANGE"]) throw std::runtime_error("Conflicting domain declarations.");
                    (key=="DOMAIN_MIN"?dmin:dmax)=vector3(line);
                }
            } else if (key.rfind("LUT_1D",0)==0) {
                throw std::runtime_error("1D/shaper and combined 1D+3D LUTs are not supported.");
            } else {
                if (!n) throw std::runtime_error("Missing LUT_3D_SIZE or unsupported directive.");
                std::istringstream values(raw); values.imbue(std::locale::classic());
                rows.push_back(vector3(values));
                if (rows.size()>size_t(n)*n*n) throw std::runtime_error("Too many LUT rows.");
            }
        } catch (const std::exception& e) {
            throw std::runtime_error("Line "+std::to_string(number)+": "+e.what());
        }
    }
    if (!n || rows.size()!=size_t(n)*n*n) throw std::runtime_error("LUT row count does not equal N cubed.");
    RGB low=rows.front(), high=low, span;
    for (const auto& v:rows) for (int c=0;c<3;++c) {low[c]=std::min(low[c],v[c]);high[c]=std::max(high[c],v[c]);}
    for (int c=0;c<3;++c) {
        span[c]=high[c]-low[c]; if (span[c]==0) span[c]=1;
        if (!(dmax[c]>dmin[c]) || !std::isfinite(span[c])) throw std::runtime_error("Invalid domain or output range.");
        // Constants are passed as float; reject domains/ranges that would collapse or overflow.
        if (!std::isfinite(float(dmin[c])) || !std::isfinite(float(dmax[c])) ||
            !(float(dmax[c])>float(dmin[c])) || !std::isfinite(float(dmax[c])-float(dmin[c])) ||
            !std::isfinite(float(low[c])) || !std::isfinite(float(high[c])) ||
            !std::isfinite(float(span[c])) || !(float(span[c])/65535.0f>0))
            throw std::runtime_error("Domain/output values exceed supported float precision.");
    }
    entry.width=2*n*n;entry.height=n;
    entry.rgba.resize(size_t(entry.width)*n*4,255);
    for (int b=0;b<n;++b) for (int g=0;g<n;++g) for (int r=0;r<n;++r) {
        const auto& value=rows[(b*n+g)*n+r];
        size_t pixel=size_t(g)*entry.width+b*n+r;
        for (int c=0;c<3;++c) {
            int q=int(std::floor(std::clamp((value[c]-low[c])/span[c],0.0,1.0)*65535.0+0.5));
            entry.rgba[4*pixel+c]=static_cast<unsigned char>(q>>8);
            entry.rgba[4*(pixel+n*n)+c]=static_cast<unsigned char>(q&255);
        }
    }
    entry.metadata[0]=n;
    for (int c=0;c<3;++c) {
        entry.metadata[1+c]=dmin[c];entry.metadata[4+c]=dmax[c];
        entry.metadata[7+c]=low[c];entry.metadata[10+c]=span[c];
    }
}

std::shared_ptr<Entry> load(const char* path, int refresh, int owner) {
    auto filename=wide(path);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(filename.c_str(),GetFileExInfoStandard,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))
        throw std::runtime_error("External .cube file is missing or inaccessible; no preset fallback.");
    uint64_t size=(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    uint64_t stamp=(uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime;
    if (!size || size>max_file_bytes) throw std::runtime_error("File must be nonempty and at most 128 MiB.");
    std::lock_guard<std::mutex> lock(mutex);
    auto key=std::make_pair(filename,owner);
    auto found=cache.find(key);
    if (found!=cache.end() && found->second->size==size && found->second->stamp==stamp && found->second->refresh==refresh) {
        found->second->used=++sequence;
        return found->second;
    }
    auto entry=std::make_shared<Entry>();entry->size=size;entry->stamp=stamp;entry->used=++sequence;
    entry->refresh=refresh;
    try {
        // Do not allow concurrent writes while taking the snapshot used for parsing.
        HANDLE handle=CreateFileW(filename.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if (handle==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open .cube for reading (possibly being written).");
        struct Close {HANDLE h;~Close(){CloseHandle(h);}} close{handle};
        BY_HANDLE_FILE_INFORMATION actual{};
        if (!GetFileInformationByHandle(handle,&actual) || actual.nFileSizeHigh!=info.nFileSizeHigh ||
            actual.nFileSizeLow!=info.nFileSizeLow || CompareFileTime(&actual.ftLastWriteTime,&info.ftLastWriteTime)!=0)
            throw std::runtime_error("File changed during loading; toggle reload to retry.");
        std::string text(size,'\0');DWORD bytes=0;
        if (!ReadFile(handle,text.data(),DWORD(size),&bytes,nullptr) || bytes!=size) throw std::runtime_error("Incomplete .cube read.");
        if (text.find('\0')!=std::string::npos) throw std::runtime_error("Expected UTF-8/ASCII text, not UTF-16 or binary.");
        parse(text,*entry);
    } catch(const std::exception& e) {entry->error=e.what();}
    cache[key]=entry;
    while (cache.size()>4) {
        auto oldest=std::min_element(cache.begin(),cache.end(),[](const auto& a,const auto& b){return a.second->used<b.second->used;});
        cache.erase(oldest);
    }
    return entry;
}

void load_cube(SCRIPT_MODULE_PARAM* param) {
    try {
        if (param->get_param_num()!=3) throw std::runtime_error("Expected file path, reload token and effect ID.");
        returned=load(param->get_param_string(0),param->get_param_int(1),param->get_param_int(2));
        if (!returned->error.empty()) throw std::runtime_error(returned->error);
        param->push_result_data(returned->rgba.data());
        param->push_result_int(returned->width);
        param->push_result_int(returned->height);
        param->push_result_array_double(returned->metadata.data(),int(returned->metadata.size()));
    } catch(const std::exception& e) {
        std::string error="FilmPrintLUT: ";error+=e.what();param->set_error(error.c_str());
    } catch(...) {param->set_error("FilmPrintLUT: unexpected loader failure.");}
}
SCRIPT_MODULE_FUNCTION functions[]={{L"load",load_cube},{nullptr,nullptr}};
SCRIPT_MODULE_TABLE table={L"FilmPrintLUT Cube Loader v1.1.1",functions};
}
extern "C" __declspec(dllexport) SCRIPT_MODULE_TABLE* GetScriptModuleTable(){return &table;}
extern "C" __declspec(dllexport) DWORD RequiredVersion(){return 2003700;}
