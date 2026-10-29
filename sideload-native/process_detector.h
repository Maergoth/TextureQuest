#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bg {
struct ProcessEvent { uint32_t pid, parent; };
struct ScanFailure { const char* stage="none"; uint32_t error=0, close_error=0; };
inline bool ProcessCovered(size_t size,size_t at,size_t width) noexcept {
    return at<=size && width<=size-at;
}
template<size_t N> inline bool ProcessName(const wchar_t (&name)[N],bool& match) noexcept {
    constexpr wchar_t wanted[]=L"everquest2.exe";
    size_t length=0;while(length<N&&name[length])++length;
    if(length==N)return false;
    match=length==sizeof(wanted)/sizeof(wanted[0])-1;
    for(size_t i=0;match&&i<length;++i){wchar_t c=name[i];if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';if(c!=wanted[i])match=false;}
    return true;
}
// Publish only a complete, bounded, known-exhausted and checked-closed snapshot.
// Fields and the entire fixed filename array must be covered before any access.
template<class Port> bool ProcessRows(Port& port,std::vector<ProcessEvent>& output,ScanFailure& failure,
                                    size_t max_rows=65536,size_t max_candidates=256) {
    failure={};
    if(!port.Open()){failure={"CreateToolhelp32Snapshot",static_cast<uint32_t>(port.LastError())};return false;}
    std::vector<ProcessEvent> staged;bool okay=true;
    try {
        typename Port::Entry row{};row.dwSize=sizeof(row);size_t count=0;
        bool present=port.First(row);
        if(!present&&port.LastError()!=Port::NoMore){failure={"Process32FirstW",static_cast<uint32_t>(port.LastError())};okay=false;}
        while(okay&&present){
            if(++count>max_rows){failure={"ProcessRowBound",122};okay=false;break;}
            if(!ProcessCovered(row.dwSize,offsetof(typename Port::Entry,th32ProcessID),sizeof(row.th32ProcessID))||
               !ProcessCovered(row.dwSize,offsetof(typename Port::Entry,th32ParentProcessID),sizeof(row.th32ParentProcessID))||
               !ProcessCovered(row.dwSize,offsetof(typename Port::Entry,szExeFile),sizeof(row.szExeFile))){failure={"ProcessRowCoverage",13};okay=false;break;}
            bool match=false;if(!ProcessName(row.szExeFile,match)){failure={"ProcessNameTerminator",13};okay=false;break;}
            if(match){
                if(!row.th32ProcessID){failure={"ProcessCandidatePID",13};okay=false;break;}
                bool duplicate=false;for(const auto& old:staged)if(old.pid==row.th32ProcessID){duplicate=true;break;}
                if(duplicate){failure={"ProcessCandidateDuplicate",13};okay=false;break;}
                if(staged.size()>=max_candidates){failure={"ProcessCandidateBound",122};okay=false;break;}
                staged.push_back({row.th32ProcessID,row.th32ParentProcessID});
            }
            row.dwSize=sizeof(row);present=port.Next(row);
            if(!present&&port.LastError()!=Port::NoMore){failure={"Process32NextW",static_cast<uint32_t>(port.LastError())};okay=false;}
        }
    } catch(...) { failure={"ProcessSnapshotStorage",8};okay=false; }
    if(!port.Close()){
        const uint32_t error=static_cast<uint32_t>(port.LastError());
        if(okay)failure={"ProcessSnapshotClose",error};else failure.close_error=error;
        okay=false;
    }
    if(okay)output.swap(staged);
    return okay;
}
}
