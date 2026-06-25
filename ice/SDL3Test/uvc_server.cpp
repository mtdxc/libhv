/*
 * Copyright (c) 2016-present The ZLMediaKit project authors. All Rights Reserved.
 *
 * This file is part of ZLMediaKit(https://github.com/ZLMediaKit/ZLMediaKit).
 *
 * Use of this source code is governed by MIT-like license that can be found in the
 * LICENSE file in the root of the source tree. All contributing project authors
 * may be found in the AUTHORS file in the root of the source tree.
 */

#include <iostream>
#include "config.h"
#include "onceToken.h"
#include "rtc/RtcHttpServer.h"
#include "Frame.h"
#include "AvCapture.h"
#include <iostream>

using namespace std;
using namespace ice;
using namespace mediakit;

#define THEAD_COUNT "general.threads"
namespace Camera {
#define CAMERA_FIELD "camera."
const string kWidth = CAMERA_FIELD"width";
const string kHeight = CAMERA_FIELD"height";
const string kFramerate = CAMERA_FIELD"framerate";
const string kBitrate = CAMERA_FIELD"bitrate";
const string kDevice = CAMERA_FIELD"device";
onceToken token1([](){
    mINI::Instance()[kDevice] = 0;
    mINI::Instance()[kWidth] = 800;
    mINI::Instance()[kHeight] = 600;
    mINI::Instance()[kFramerate] = 30;
    mINI::Instance()[kBitrate] = 500000;
}, nullptr);
} // namespace Camera 

namespace Microphone {
#define MIC_FIELD "mic."
const string kSamplerate = MIC_FIELD"samplerate";
const string kChannel = MIC_FIELD"channels";
const string kBitrate = MIC_FIELD"bitrate";
const string kDevice = MIC_FIELD"device";
onceToken token1([](){
    mINI::Instance()[kDevice] = 0;
    mINI::Instance()[kSamplerate] = 44100;
    mINI::Instance()[kChannel] = 1;
    mINI::Instance()[kBitrate] = 64000;
}, nullptr);
} // namespace Microphone 

// //////////HTTP配置///////////
namespace Http {
#define HTTP_FIELD "http."
const string kPort = HTTP_FIELD"port";
const string kSSLPort = HTTP_FIELD"sslport";
const string kTcpPort = HTTP_FIELD"tcpport";
const string kUdpPort = HTTP_FIELD"udpport";
onceToken token1([](){
    mINI::Instance()[kPort] = 8080;
    mINI::Instance()[kSSLPort] = 8443;
    mINI::Instance()[kTcpPort] = 9000;
    mINI::Instance()[kUdpPort] = 9000;
    mINI::Instance()[THEAD_COUNT] = std::thread::hardware_concurrency();
},nullptr);
}//namespace Http

int main(int argc,char *argv[]) {
    //loadIniConfig();
    logger_enable_color(hlog, true);
    hlog_set_handler(stdout_logger);
    size_t threads = mINI::Instance()[THEAD_COUNT];

    RtcHttpConfig config;
    config.ice.gatherTcp = true;
    config.ice.tcpPort = mINI::Instance()[Http::kTcpPort];
    config.ice.udpPort = mINI::Instance()[Http::kUdpPort];
    config.http_port = mINI::Instance()[Http::kPort];
    config.https_port = mINI::Instance()[Http::kSSLPort];
    config.cert_file = "cert/server.crt";
    config.key_file = "cert/server.key";

    RtcHttpServer server(config);
    server.start();

    std::string stream = "uvc";
    if (argc > 1) {
        stream = argv[1];
    }        
    auto capture = std::make_shared<AvCapture>();
    capture->PrintDevices();
    
    GET_CONFIG(uint32_t, device, Microphone::kDevice);
    GET_CONFIG(int, samplerate, Microphone::kSamplerate);
    GET_CONFIG(int, channel, Microphone::kChannel);
    capture->setupAudio(CodecAAC, samplerate, channel, device);

    GET_CONFIG(uint32_t, vbitrate, Camera::kBitrate);
    GET_CONFIG(uint32_t, width, Camera::kWidth);
    GET_CONFIG(uint32_t, height, Camera::kHeight);
    GET_CONFIG(uint32_t, framerate, Camera::kFramerate);    
    capture->setupVideo(CodecH264, width, height, framerate);

    server.setDispatcher(stream, capture);
    printf("press q to quit loop\n");
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        if (!strcasecmp(line, "q\n") || !strcasecmp(line, "quit\n")) {
            printf("user break loop\n");
            break;
        }
    }
    server.stop();
    return 0;
}

