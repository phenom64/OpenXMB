#include "openxmb/desktop/http.hpp"
#include <iostream>
#include <stdexcept>
using namespace openxmb::desktop;
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
int main() {
  try {
    for(const auto& address:{std::string{},std::string("https://example.invalid/\r\nsecret"),std::string("https://example.invalid/")+char{}+"secret"}) {
      const auto response=http_get({address});require(!response.success && response.body.empty(),"Invalid addresses must fail without a response body");
      require(response.error.find("secret")==std::string::npos,"Network validation leaked the address");
    }
    HttpRequest request{"https://example.invalid/secret",{"Authorization: Bearer secret"}};request.sensitive=true;request.max_redirects=1;
    auto response=http_get(request);require(!response.success && response.error.find("secret")==std::string::npos,"Authenticated redirects were allowed or leaked credentials");
    request.max_redirects=0;request.headers={"Authorization: Bearer secret\r\nInjected: value"};
    require(!http_get(request).success,"Header injection must be rejected");
    request.headers={};request.max_bytes=0;require(!http_get(request).success,"Unbounded downloads must be rejected");
    request.max_bytes=256u*1024u*1024u+1;require(!http_get(request).success,"Oversized download allocation must be rejected");
    std::cout<<"Bounded HTTPS and credential handling checks passed\n";
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
