#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

static void send_all(int fd, const std::string& data) { for (size_t at=0;at<data.size();) { auto n=send(fd,data.data()+at,data.size()-at,0); if(n<=0) throw std::runtime_error("WebSocket send failed"); at+=n; } }
static std::string receive_exact(int fd, size_t size) { std::string out(size, '\0'); for(size_t at=0;at<size;) { auto n=recv(fd,out.data()+at,size-at,0); if(n<=0) throw std::runtime_error("WebSocket closed before expected frame"); at+=n; } return out; }
static std::pair<unsigned,std::string> frame(int fd) { auto h=receive_exact(fd,2); size_t n=unsigned(h[1])&127; if(n==126){auto x=receive_exact(fd,2);n=(unsigned((unsigned char)x[0])<<8)|(unsigned char)x[1];} else if(n==127) throw std::runtime_error("unexpected oversized frame"); return {unsigned(h[0])&15,receive_exact(fd,n)}; }
static void exchange(unsigned opcode, const std::string& payload, const std::vector<std::pair<unsigned,std::string>>& expected) { int fd=socket(AF_INET,SOCK_STREAM,0); sockaddr_in addr{};addr.sin_family=AF_INET;addr.sin_port=htons(80);inet_pton(AF_INET,"127.0.0.1",&addr.sin_addr);if(connect(fd,(sockaddr*)&addr,sizeof(addr)))throw std::runtime_error("WebSocket connect failed"); send_all(fd,"GET /tests/capy-websocket.capy HTTP/1.1\r\nHost: bearer.openfu.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: MTIzNDU2Nzg5MDEyMzQ1Ng==\r\nSec-WebSocket-Version: 13\r\n\r\n");std::string response;while(response.find("\r\n\r\n")==std::string::npos)response+=receive_exact(fd,1);if(!response.starts_with("HTTP/1.1 101 "))throw std::runtime_error("Capy WebSocket upgrade failed");std::string f;f.push_back(char(128|opcode));f.push_back(char(128|payload.size()));const std::array<unsigned char,4> mask={0x12,0x34,0x56,0x78};f.append((const char*)mask.data(),4);for(size_t i=0;i<payload.size();++i)f.push_back(payload[i]^mask[i%4]);send_all(fd,f);for(auto& want:expected)if(frame(fd)!=want)throw std::runtime_error("Capy WebSocket frame mismatch");close(fd); }
int main(){try{exchange(1,"hello",{{1,"echo:hello"},{1,"direct:hello"},{8,std::string("\x03\xe8",2)}});exchange(1,"flush",{{1,"flush-rejected"},{8,std::string("\x03\xe8",2)}});exchange(2,std::string("\0\xff" "capy",6),{{2,std::string("\0\xff" "capy",6)},{8,std::string("\x03\xe8",2)}});}catch(const std::exception& e){write(2,e.what(),std::strlen(e.what()));write(2,"\n",1);return 1;}return 0;}
