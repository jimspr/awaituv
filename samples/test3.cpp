// Test3.cpp : Example using CURL to access petstore service
//

#include <awaitcurl.h>
#include <string>

using namespace std;
using namespace awaituv;
using namespace awaitcurl;

future_t<void> test3(curl_requester_t& requester)
{
  auto resourcetype = "pet";
  auto resourcelink = "pet/1";
  auto url = std::string{ "https://petstore.swagger.io/v2/" } + resourcelink;

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/json");

  auto curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

  char buffer[64];
  auto result = co_await requester.invoke(curl);
  if (result.curl_code == CURLE_OK) {
    sprintf(buffer, "http_code: %ld\n", result.http_code);
    string_buf_t buf{ buffer, strlen(buffer) };
    fs_t         writereq;
    (void)co_await fs_write(uv_default_loop(), &writereq, 1 /*stdout*/, &buf,
                            1, -1);

    auto&        str = result.str;
    string_buf_t buf2{ str.c_str(), str.size() };
    (void)co_await fs_write(uv_default_loop(), &writereq, 1 /*stdout*/, &buf2,
                            1, -1);
  } else {
    sprintf(buffer, "failed %d\n", result.curl_code);
    string_buf_t buf{ buffer, strlen(buffer) };
    fs_t         writereq;
    (void)co_await fs_write(uv_default_loop(), &writereq, 1 /*stdout*/, &buf,
                            1, -1);
  }

  curl_slist_free_all(headers);
  co_return;
}

int main(int argc, char* argv[])
{
  if (curl_global_init(CURL_GLOBAL_ALL))
    return -1;

  {
    curl_requester_t requester(*uv_default_loop());

    test3(requester);

    uv_run(uv_default_loop(), UV_RUN_DEFAULT);
  }

  curl_global_cleanup();
  uv_loop_close(uv_default_loop());

  return 0;
}
