#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>
#include "routing.h"
#include "sha256.h"

namespace tq {
inline bool Environment(const wchar_t* name,std::wstring& value) {
    wchar_t buffer[kPathChars];DWORD n=GetEnvironmentVariableW(name,buffer,static_cast<DWORD>(kPathChars));
    if(n==0||n>=kPathChars)return false;value.assign(buffer,n);return true;
}
inline bool HashText(const std::wstring& text,std::string& hash) {
    if(text.size()!=64)return false;hash.clear();
    for(wchar_t c:text){if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')))return false;hash.push_back(static_cast<char>(c));}return true;
}
inline bool ReadManifestFile(const std::wstring& path,const std::string& expected,std::vector<unsigned char>& bytes,DWORD& error) {
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE){error=GetLastError();return false;}
    LARGE_INTEGER length{};bool okay=GetFileSizeEx(h,&length)&&length.QuadPart>=16&&length.QuadPart<=kMaxManifest;
    if(!okay)error=GetLastError()?GetLastError():ERROR_FILE_INVALID;
    if(okay){bytes.resize(static_cast<size_t>(length.QuadPart));DWORD read=0;okay=ReadFile(h,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)&&read==bytes.size();if(!okay)error=GetLastError()?GetLastError():ERROR_HANDLE_EOF;}
    if(okay){unsigned char extra=0;DWORD read=0;okay=ReadFile(h,&extra,1,&read,nullptr)&&read==0;if(!okay)error=ERROR_FILE_INVALID;}
    if(!CloseHandle(h)){error=GetLastError();okay=false;}
    if(okay&&Sha256(bytes.data(),bytes.size())!=expected){error=ERROR_CRC;okay=false;}
    return okay;
}
inline bool UnderPackage(const std::wstring& path,const std::wstring& package,std::wstring& canonical) {
    std::wstring root,data;return Canonical(path,canonical)&&Canonical(package,root)&&ComponentDataRoot(root,data)&&Prefix(canonical.c_str(),root);
}
}
