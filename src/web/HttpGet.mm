// HTTPS GET for the guest feed (web/Guests.hpp): NSURLSession, blocking the calling (feed) thread.

#import <Foundation/Foundation.h>

#include "web/Guests.hpp"

namespace reforce::web {

std::optional<std::string> httpGet(const std::string& url, double timeoutSeconds, std::string* error,
                                   const std::string& bearer) {
    @autoreleasepool {
        NSURL* u = [NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]];
        if (!u) {
            if (error) *error = "bad URL";
            return std::nullopt;
        }
        NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u
                                                           cachePolicy:NSURLRequestReloadIgnoringLocalCacheData
                                                       timeoutInterval:timeoutSeconds];
        [req setValue:@"reforce" forHTTPHeaderField:@"User-Agent"];
        if (!bearer.empty())
            [req setValue:[@"Bearer " stringByAppendingString:[NSString stringWithUTF8String:bearer.c_str()]]
                forHTTPHeaderField:@"Authorization"];
        __block std::optional<std::string> body;
        __block std::string err;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        NSURLSessionDataTask* task = [[NSURLSession sharedSession]
            dataTaskWithRequest:req
              completionHandler:^(NSData* data, NSURLResponse* response, NSError* e) {
                  const NSInteger status =
                      [response isKindOfClass:[NSHTTPURLResponse class]] ? ((NSHTTPURLResponse*)response).statusCode : 0;
                  if (e) err = e.localizedDescription.UTF8String ?: "request failed";
                  else if (status < 200 || status >= 300) err = "HTTP " + std::to_string(status);
                  else body = std::string(static_cast<const char*>(data.bytes), data.length);
                  dispatch_semaphore_signal(done);
              }];
        [task resume];
        dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, int64_t((timeoutSeconds + 2) * NSEC_PER_SEC)));
        if (!body && error) *error = err.empty() ? "timed out" : err;
        return body;
    }
}

}  // namespace reforce::web
