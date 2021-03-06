#include "natty.h"

#include "webrtc/base/thread.h"
#include "webrtc/base/logging.h"
#include "flagdefs.h"

#include <iostream>
#include <string>

static const int LOG_DEFAULT = rtc::LS_INFO;

using namespace std;

int main(int argc, char* argv[]) {

  rtc::FlagList::SetFlagsFromCommandLine(&argc, argv, true);
  if (FLAG_help) {
      rtc::FlagList::Print(NULL, false);
    return 0;
  }

  if (FLAG_debug) {
    rtc::LogMessage::LogTimestamps();
    rtc::LogMessage::LogToDebug(LOG_DEFAULT);
  }

  /* Set up the crypto subsystem. */
  rtc::InitializeSSL();
  rtc::Thread* thread = rtc::Thread::Current();

  /* Scopers help maintain ownership of a pointer
   */
  rtc::scoped_refptr<Natty> natty(
      new rtc::RefCountedObject<Natty>(thread));

  natty.get()->Init(FLAG_offer, FLAG_out, FLAG_stuns); 
  
  natty.get()->ProcessInput();
  return 0;
}

