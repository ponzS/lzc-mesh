#include "app.h"
#include "identity.h"
#include "../../transport/include.h"
#include <wincrypt.h>
#include <shlobj.h>
#include <vector>

namespace identity {
namespace {
MeshIdentity *key=nullptr;
std::string take(char *text) {std::string value=text?text:"";mesh_identity_string_free(text);return value;}
bool fail(const wchar_t *why) {setStatus(S_TRUST,L_ERROR,std::wstring(why)+L"；共享已停止，未重置设备凭证");return false;}
}
bool init() {
    if(key)return true;
    wchar_t home[MAX_PATH];DWORD size=GetEnvironmentVariableW(L"LOCALAPPDATA",home,MAX_PATH);
    if(!size || size>=MAX_PATH)return fail(L"无法定位凭证目录");
    const auto dir=std::wstring(home,size)+L"\\LZC Mesh";
    SHCreateDirectoryExW(nullptr,dir.c_str(),nullptr);
    const auto path=dir+L"\\device-key.bin";
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if(file!=INVALID_HANDLE_VALUE) {
        BY_HANDLE_FILE_INFORMATION info{};DWORD read=0;
        const bool valid=GetFileInformationByHandle(file,&info) && !(info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY)) && info.nNumberOfLinks==1 && !info.nFileSizeHigh && info.nFileSizeLow>0 && info.nFileSizeLow<16384;
        std::vector<BYTE> bytes(valid?info.nFileSizeLow:0);
        const bool ok=valid && ReadFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr) && read==bytes.size();
        CloseHandle(file);
        if(!ok)return fail(L"无法安全读取设备凭证");
        DATA_BLOB encrypted{static_cast<DWORD>(bytes.size()),bytes.data()},plain{};
        if(!CryptUnprotectData(&encrypted,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&plain))return fail(L"无法解密原有设备凭证");
        std::string stored(reinterpret_cast<char *>(plain.pbData),plain.cbData);
        SecureZeroMemory(plain.pbData,plain.cbData);LocalFree(plain.pbData);
        if(stored.empty() || stored.find('\0')!=std::string::npos){SecureZeroMemory(stored.data(),stored.size());return fail(L"设备凭证无效");}
        key=mesh_identity_new(stored.c_str());SecureZeroMemory(stored.data(),stored.size());
        if(!key)return fail(L"原有设备凭证已损坏");
    } else {
        if(GetLastError()!=ERROR_FILE_NOT_FOUND)return fail(L"无法打开设备凭证");
        auto *created=mesh_identity_new(nullptr);if(!created)return fail(L"无法生成设备凭证");
        auto stored=take(mesh_identity_export(created));
        DATA_BLOB plain{static_cast<DWORD>(stored.size()),reinterpret_cast<BYTE *>(stored.data())},encrypted{};
        const bool protectedOk=CryptProtectData(&plain,L"LZC Mesh device identity",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&encrypted);
        SecureZeroMemory(stored.data(),stored.size());
        if(!protectedOk){mesh_identity_free(created);return fail(L"无法保护设备凭证");}
        file=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        bool ok=false;
        if(file!=INVALID_HANDLE_VALUE) {
            DWORD written=0;ok=WriteFile(file,encrypted.pbData,encrypted.cbData,&written,nullptr) && written==encrypted.cbData && FlushFileBuffers(file);
            CloseHandle(file);if(!ok)DeleteFileW(path.c_str());
        }
        LocalFree(encrypted.pbData);
        if(!ok){mesh_identity_free(created);return fail(L"无法保存设备凭证");}
        key=created;
    }
    setStatus(S_TRUST,L_UNKNOWN,L"等待批准 · 指纹 "+widen(fingerprint()));
    return true;
}
std::string publicKey(){return key?take(mesh_identity_public(key)):"";}
std::string fingerprint(){return key?take(mesh_identity_fingerprint(key)):"";}
std::string proof(const std::string &user,const std::string &nonce){return key?take(mesh_identity_proof(key,user.c_str(),g.deviceId.c_str(),nonce.c_str())):"";}
void shutdown(){mesh_identity_free(key);key=nullptr;}
}
