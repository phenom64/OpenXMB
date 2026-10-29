#include "openxmb/desktop/http.hpp"
#include <algorithm>
#include <cstdint>
#include <memory>
#ifndef OPENXMB_VERSION_STRING
#define OPENXMB_VERSION_STRING "development"
#endif
#ifdef OPENXMB_HAVE_CURL
#include <curl/curl.h>
#endif

namespace openxmb::desktop {
HttpResponse http_get(const HttpRequest& request) {
  HttpResponse response;
  const auto invalid=[](const std::string& text) {return text.find('\0')!=std::string::npos || text.find('\r')!=std::string::npos || text.find('\n')!=std::string::npos;};
  if(request.url.empty() || request.url.size()>32768 || invalid(request.url)) {response.error="The HTTPS address is invalid.";return response;}
  if(!request.max_bytes || request.max_bytes>256u*1024u*1024u || !request.timeout_seconds || request.timeout_seconds>120 || request.max_redirects>5) {
    response.error="The network request exceeds the supported size or time limits.";return response;
  }
  if(request.sensitive && request.max_redirects) {response.error="Authenticated requests cannot follow redirects.";return response;}
  if(request.headers.size()>32 || std::ranges::any_of(request.headers,[&](const auto& value) {return value.size()>8192 || invalid(value);})) {
    response.error="A request header is invalid.";return response;
  }
#ifdef OPENXMB_HAVE_CURL
  static const auto initialized=curl_global_init(CURL_GLOBAL_DEFAULT);
  if(initialized!=CURLE_OK) {response.error="The network service could not initialize.";return response;}
  const auto release_url=[](CURLU* value){curl_url_cleanup(value);};
  const std::unique_ptr<CURLU,decltype(release_url)> url(curl_url(),release_url);
  if(!url || curl_url_set(url.get(),CURLUPART_URL,request.url.c_str(),0)!=CURLUE_OK) {response.error="The HTTPS address is invalid.";return response;}
  const auto part=[&](CURLUPart field) {
    char* text{};std::string result;
    if(curl_url_get(url.get(),field,&text,0)==CURLUE_OK) {result=text;curl_free(text);}
    return result;
  };
  if(part(CURLUPART_SCHEME)!="https" || part(CURLUPART_HOST).empty() || !part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty()) {
    response.error="Use an HTTPS address without embedded login details.";return response;
  }
  const auto release=[](CURL* value){curl_easy_cleanup(value);};
  const std::unique_ptr<CURL,decltype(release)> client(curl_easy_init(),release);
  if(!client) {response.error="The network request could not initialize.";return response;}
  struct Body {std::string* value;std::size_t limit;bool exceeded{},failed{};} body{&response.body,request.max_bytes};
  curl_easy_setopt(client.get(),CURLOPT_URL,request.url.c_str());
  curl_easy_setopt(client.get(),CURLOPT_USERAGENT,"OpenXMB/" OPENXMB_VERSION_STRING);
  curl_easy_setopt(client.get(),CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(client.get(),CURLOPT_CONNECTTIMEOUT,5L);
  curl_easy_setopt(client.get(),CURLOPT_TIMEOUT,static_cast<long>(request.timeout_seconds));
  curl_easy_setopt(client.get(),CURLOPT_PROTOCOLS_STR,"https");
  curl_easy_setopt(client.get(),CURLOPT_REDIR_PROTOCOLS_STR,"https");
  curl_easy_setopt(client.get(),CURLOPT_FOLLOWLOCATION,request.max_redirects?1L:0L);
  curl_easy_setopt(client.get(),CURLOPT_MAXREDIRS,static_cast<long>(request.max_redirects));
  curl_easy_setopt(client.get(),CURLOPT_MAXFILESIZE_LARGE,static_cast<curl_off_t>(request.max_bytes));
  curl_easy_setopt(client.get(),CURLOPT_WRITEFUNCTION,+[](char* data,std::size_t size,std::size_t count,void* context)->std::size_t {
    auto& body=*static_cast<Body*>(context);
    if(size && count>SIZE_MAX/size) {body.exceeded=true;return 0;}
    const auto bytes=size*count;
    if(bytes>body.limit-body.value->size()) {body.exceeded=true;return 0;}
    try {body.value->append(data,bytes);return bytes;} catch(...) {body.failed=true;return 0;}
  });
  curl_easy_setopt(client.get(),CURLOPT_WRITEDATA,&body);
  if(request.progress) {
    curl_easy_setopt(client.get(),CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(client.get(),CURLOPT_XFERINFOFUNCTION,+[](void* context,curl_off_t total,curl_off_t downloaded,curl_off_t,curl_off_t)->int {
      const auto& callback=*static_cast<const std::function<bool(std::uint64_t,std::uint64_t)>*>(context);
      try {return callback(static_cast<std::uint64_t>(std::max<curl_off_t>(0,downloaded)),static_cast<std::uint64_t>(std::max<curl_off_t>(0,total)))?0:1;}
      catch(...) {return 1;}
    });
    curl_easy_setopt(client.get(),CURLOPT_XFERINFODATA,&request.progress);
  }
  curl_slist* headers{};
  for(const auto& header:request.headers) {
    auto* next=curl_slist_append(headers,header.c_str());
    if(!next) {curl_slist_free_all(headers);response.error="The request headers could not be allocated.";return response;}
    headers=next;
  }
  const std::unique_ptr<curl_slist,decltype(&curl_slist_free_all)> header_owner(headers,curl_slist_free_all);
  if(headers)curl_easy_setopt(client.get(),CURLOPT_HTTPHEADER,headers);
  const auto status=curl_easy_perform(client.get());
  curl_easy_getinfo(client.get(),CURLINFO_RESPONSE_CODE,&response.status);
  if(body.exceeded || status==CURLE_FILESIZE_EXCEEDED)response.error="The response exceeds the download limit.";
  else if(body.failed)response.error="There is not enough memory for the response.";
  else if(status!=CURLE_OK)response.error=status==CURLE_ABORTED_BY_CALLBACK?"The download was canceled.":status==CURLE_OPERATION_TIMEDOUT?"The request timed out.":"The HTTPS request failed. Check the connection and try again.";
  else if(response.status<200 || response.status>=300)response.error="The server returned HTTP "+std::to_string(response.status)+".";
  else response.success=true;
  if(!response.success)response.body.clear();
#else
  response.error="This build does not include HTTPS integration support.";
#endif
  return response;
}
} // namespace openxmb::desktop
