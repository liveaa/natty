/**
 * Copyright (C) 2014 Lantern
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "natty.h"

#include <utility>
#include <iostream>
#include <fstream>
#include <streambuf>
#include <string>

#include "talk/app/webrtc/videosourceinterface.h"
#include "talk/app/webrtc/jsep.h"
#include "talk/app/webrtc/mediaconstraintsinterface.h"
#include "webrtc/base/common.h"
#include "talk/p2p/base/sessiondescription.h"
#include "talk/app/webrtc/test/fakeconstraints.h"
#include "talk/app/webrtc/peerconnectioninterface.h"
#include "talk/app/webrtc/portallocatorfactory.h"
#include "talk/session/media/mediasession.h"
#include "talk/p2p/base/constants.h"
#include "talk/app/webrtc/datachannelinterface.h"
#include "webrtc/base/json.h"
#include "talk/p2p/base/candidate.h"
#include "webrtc/base/logging.h"
#include "talk/media/devices/devicemanager.h"

using namespace std;
using namespace webrtc;

#define DEBUG(fmt, ...) printf("%s:%d: " fmt, __FILE__, __LINE__, __VA_ARGS__);

// Names used for a IceCandidate JSON object.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

typedef PeerConnectionInterface::IceServers IceServers;
typedef PeerConnectionInterface::IceServer IceServer;;
typedef PeerConnection::IceConnectionState IceConnState;

// Names used for a SessionDescription JSON object.
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";

void Natty::InputStream::read(Natty* natty) {
  while (getline(std::cin, input) || 1) {
    /* need to remove new lines or the SDP won't be valid */
    if (input == "exit") {
      natty->Shutdown();
      break;
    }

    if (input.empty()) {
      /* terminate input on empty line */
      //std::cout << "\n";
      continue;
    }
    natty->ReadMessage(input);  
  }

}

string Natty::InputStream::build() const {
  string str = ss.str();
  str.erase(
      std::remove(str.begin(), str.end(), '\n'), str.end()
      ); 
  return str;
}

Natty::Natty(rtc::Thread* thread
    )
: thread_(thread) {
  }

Natty::~Natty() {
  if (outfile) {
    outfile.close();
  }
  ASSERT(peer_connection_.get() == NULL);
}

bool Natty::connection_active() const {
  return peer_connection_.get() != NULL;
}

std::string GetEnvVarOrDefault(const char* env_var_name,
    const char* default_value) {
  std::string value;
  const char* env_var = getenv(env_var_name);
  if (env_var)
    value = env_var;

  if (value.empty())
    value = default_value;

  return value;
}

std::string GetPeerConnectionString() {
  return GetEnvVarOrDefault("WEBRTC_CONNECT", "stun:stun.l.google.com:19302");
}


bool Natty::InitializePeerConnection() {

  IceServers servers;
  IceServer server;
  FakeConstraints constraints;
  webrtc::InternalDataChannelInit dci;
  const string& dc_name = "datachannel";

  // Enable RTP DataChannels
  //constraints.SetAllowRtpDataChannels();
  // Enable DTLS-SRTP
  // http://tools.ietf.org/html/rfc5764
  constraints.AddOptional(
      webrtc::MediaConstraintsInterface::kEnableDtlsSrtp, true);

  ASSERT(peer_connection_factory_.get() == NULL);
  ASSERT(peer_connection_.get() == NULL);

  peer_connection_factory_  = CreatePeerConnectionFactory();

  if (!peer_connection_factory_.get()) {
    LOG(INFO) << "Failed to initialize peer connection factory";
    Shutdown();
    return false;
  }
  LOG(INFO) << "Created peer connection factory";

  server.uri = GetPeerConnectionString();
  servers.push_back(server);

  /* Creating a peer connection object is when
   * we start to generate ICE candidates
   */
  peer_connection_ = peer_connection_factory_->CreatePeerConnection(servers, &constraints, NULL, NULL, this);
  dci.reliable = true;
  data_channel_ = peer_connection_->CreateDataChannel(dc_name, &dci);
  data_channel_observer_ = new NattyDataChannelObserver(data_channel_);

  if (!peer_connection_.get()) {
    LOG(INFO) << "Create peer connection failed";
    Shutdown();
  }

  LOG(INFO) << "Created peer connection";
  return peer_connection_.get() != NULL;
}

void Natty::Shutdown() {
  LOG(INFO) << "Deleting peer connection";
  data_channel_->Close();
  peer_connection_->Close();
  peer_connection_ = NULL;
  peer_connection_factory_ = NULL;
  //active_streams_.clear();
  rtc::CleanupSSL();
  outfile.close();
  //thread_->Stop();
}

//
// PeerConnectionObserver implementation.
//

void Natty::OnError() {
  LOG(INFO) << "Peer connection error encountered";
  LOG(INFO) << __FUNCTION__;
}

void Natty::OnAddStream(MediaStreamInterface* stream) {
  LOG(INFO) << "Successfully added stream";
  stream->AddRef();
}

/* the answerer removes his media stream before disconnecting
 * this is triggered when that happens and means we
 * were able to communicate 
 */
void Natty::OnRemoveStream(MediaStreamInterface* stream) {
  LOG(INFO) << "Successfully removed stream";
  stream->AddRef(); 
  //PickFinalCandidate();
}

void Natty::OnSignalingChange(PeerConnectionInterface::SignalingState new_state) {
  LOG(INFO) << "Signaling state change";
}

void Natty::OnStateChange(
    PeerConnectionObserver::StateType state_changed) {
}

/* Once an ice candidate have been found PeerConnection will call the
 * observer function OnIceCandidate. The candidate is serialized
 * to be sent to the remote peer.
 */
void Natty::OnIceCandidate(const IceCandidateInterface* candidate) {
  Json::FastWriter writer;
  Json::Value jmessage;

  jmessage[kCandidateSdpMidName] = candidate->sdp_mid();
  jmessage[kCandidateSdpMlineIndexName] = candidate->sdp_mline_index();
  std::string sdp;
  if (!candidate->ToString(&sdp)) {
    LOG(LS_ERROR) << "Failed to serialize candidate";
    return;
  }

  jmessage[kCandidateSdpName] = sdp;
  outfile << writer.write(jmessage);
  outfile.flush();
}

/* ICE observer that signals when our connection state changes
 * "Completed" means ICE has finished gathering and checking and found a connection
 * for all components.
 *
 */
void Natty::OnIceConnectionChange(IceConnState new_state) {
  //LOG(INFO) << "New connection state: " << connection_states[new_state];
  if (new_state == PeerConnection::kIceConnectionCompleted ||
      new_state == PeerConnection::kIceConnectionClosed) {
    /* The ICE agent has finished gathering and checking and found a connection
     * for all components.
     */
    LOG(INFO) << "Found ideal connection";
    //Shutdown();
  } else if (new_state == PeerConnection::kIceConnectionFailed) {
    const std::string& msg = "Checked all candidate pairs and failed to find a connection";
    LOG(INFO) << msg;
    OnFailure(msg);
    Shutdown();
  }
}

void Natty::OnRenegotiationNeeded() {
  LOG(INFO) << "Renegotiation needed";
}

/* New natty JSON arrived on stdin
 * if type is defined, we have an SDP message
 * otherwise, it's a remote ICE candidate 
 */
void Natty::ReadMessage(const std::string& message) {
  Json::Reader reader;
  Json::Value jmessage;
  std::string type;
  std::string json_object;

  if (!reader.parse(message, jmessage)) {
    LOG(INFO) << "Received an unknown message.";
    return;
  }
  GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName, &type);

  if (!type.empty()) {
    std::string sdp;
    if (!GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName, &sdp)) {
      LOG(INFO) << "Can't parse received session description message.";
      return;
    }
    session_description = CreateSessionDescription(type, sdp);
    if (!session_description) {
      LOG(INFO) << "Can't parse SDP message";
      return;
    }
    LOG(INFO) << "Received session description " << message << " sending answer back";

    peer_connection_->SetRemoteDescription(
        NattySessionObserver::Create(), session_description);
    if (session_description->type() ==
        SessionDescriptionInterface::kOffer) {
      peer_connection_->CreateAnswer(this, NULL);
      LOG(INFO) << "signaling state " << peer_connection_->signaling_state();
    }
    return;
  }
  else {
    std::string sdp_mid;
    int sdp_mlineindex = 0;
    std::string sdp;
    if (!GetStringFromJsonObject(jmessage, kCandidateSdpMidName, &sdp_mid) ||
        !GetIntFromJsonObject(jmessage, kCandidateSdpMlineIndexName,
          &sdp_mlineindex) ||
        !GetStringFromJsonObject(jmessage, kCandidateSdpName, &sdp)) {
      LOG(INFO) << "Can't parse received message";
      return;
    }
    rtc::scoped_ptr<IceCandidateInterface> candidate(
        CreateIceCandidate(sdp_mid, sdp_mlineindex, sdp));
    LOG(INFO) << "Remote candidate information";

    if (!candidate.get()) {
      LOG(WARNING) << "Can't parse received candidate message.";
      return;
    }
    if (!peer_connection_->AddIceCandidate(candidate.get())) {
      LOG(WARNING) << "Failed to apply the received candidate";
      return;
    }
    LOG(INFO) << candidate.get()->candidate().ToString();
    LOG(INFO) << " Received candidate :" << message;
    return;
  }
};

/* Grabs the transport channel from the session description 
 *
 * Note from source: The Session object hosts the P2PTransport object and
 * requests creation of data channels. Although the Session object can
 * potentially host multiple instances and subclasses of Transport objects, the
 * current version of the code only defines and uses one instance of the
 * P2PTransport subclass.
 *
 */
void Natty::InspectTransportChannel() {
  const SessionDescriptionInterface* remote = 
      peer_connection_->remote_description();
  const cricket::SessionDescription* desc = remote->description();
  const cricket::TransportInfos transport_infos = desc->transport_infos();
  for (size_t i = 0; i < transport_infos.size(); ++i) {
    const cricket::TransportInfo transport1 = transport_infos.at(i);
    cricket::TransportDescription transport_desc = transport1.description;
    LOG(INFO) << "transport desc connection role " << transport_desc.connection_role;
    typedef std::vector<cricket::Candidate> Candidates;
    Candidates cands = transport_desc.candidates;
    for (size_t j = 0; j < cands.size(); ++j) {
      LOG(INFO) << "candindate -> " << cands.at(j).ToString() << "\n";
    }
  }
}

/* This function is used to emit 5-tuples within the
 * P2PTransportChannel to stdout
 *
 * P2PTransportChannel represents a data channel between the local and remote
 * computers. This channel actually obscures a complex system designed for
 * robustness and performance. P2PTransportChannel manages a number of different
 * Connection objects, each of which is specialized for a different connection
 * type (UDP, TCP, etc). A Connection object actually wraps a pair of objects: a
 * Port subclass, representing the local connection; and an address representing
 * the remote connection. If a particular connection fails, P2PTransportChannel
 * will seamlessly switch to the next best connection.
 */

/* Used when ICE has checked all candidate pairs
 * and failed to find a connection for at least one
 */
void Natty::OnFailure(const std::string& msg) {
  Json::FastWriter writer;
  Json::Value jmessage;
  jmessage["type"] = "error";
  jmessage["message"] = msg;
  outfile << writer.write(jmessage);
  outfile.flush();
}

void Natty::OnDataChannel(DataChannelInterface* data_channel) {
  LOG(INFO) << "New data channel created " << data_channel;
}

void Natty::OnIceComplete() {
  LOG(INFO) << "ICE finished gathering candidates!";
}

// PeerConnectionObserver implementation.
void Natty::OnServerConnectionFailure() {

}

std::string GetPeerName() {
  char computer_name[256];
  if (gethostname(computer_name, ARRAY_SIZE(computer_name)) != 0)
    strcpy(computer_name, "host");
  std::string ret(GetEnvVarOrDefault("USERNAME", "user"));
  ret += '@';
  ret += computer_name;
  return ret;
}   

void Natty::setMode(Natty::Mode m) {
  mode = m;
}

void Natty::InitConnectionStates() {
  const string states[] = {"New", "Checking", "Connected", "Completed",
    "Failed", "Disconnected", "Closed"};
  for ( int i = PeerConnection::kIceConnectionNew; 
       i != PeerConnection::kIceConnectionClosed; i++ )
  {
    ConnState state = static_cast<ConnState>(i);
    connection_states[state] = states[i].c_str();
  }

}

void Natty::Init(bool offer) {
  InitConnectionStates();
  InitializePeerConnection();
  if (offer) {
    Natty::setMode(Natty::OFFER);
    /* extraneous already set in peer connection factory */
    FakeConstraints constraints;
    constraints.AddOptional(
        webrtc::MediaConstraintsInterface::kEnableDtlsSrtp, false);
    peer_connection_->CreateOffer(this, &constraints);
  }
}

void Natty::ProcessInput() {
  Natty::InputStream is;
  is.read(this);

}

/* need to update this to accept stdout when the stdout
 * option is blank 
 *
 */
void Natty::OpenDumpFile(const std::string& filename) {
  if (!filename.empty()) {
    outfile.open(filename.c_str());
  }
  else {
    outfile.copyfmt(std::cout);
    outfile.clear(std::cout.rdstate());
    outfile.basic_ios<char>::rdbuf(std::cout.rdbuf());
  }
}

/* Jsep CreateOffer and CreateAnswer on success callback
 * 
 * the generated blob of SDP data contains session information
 * and configuration
 * this is called after createOffer and createAnswer
 */
void Natty::OnSuccess(SessionDescriptionInterface* desc) {
  LOG(INFO) << "Setting local description";
  peer_connection_->SetLocalDescription(
      NattySessionObserver::Create(), desc);
  Json::FastWriter writer;
  Json::Value jmessage;
  jmessage[kSessionDescriptionTypeName] = desc->type();

  std::string sdp;
  desc->ToString(&sdp);
  jmessage[kSessionDescriptionSdpName] = sdp;
  outfile << writer.write(jmessage); 
  outfile.flush();
}
