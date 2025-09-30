#ifndef BAIDU_GOOGLE_PROTOCOL_H
#define BAIDU_GOOGLE_PROTOCOL_H

#include "protocol.h"
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

class BaiduGoogleProtocol : public Protocol {
public:
    BaiduGoogleProtocol();
    ~BaiduGoogleProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    bool is_audio_channel_opened_;
    std::string session_id_;
    std::string baidu_access_token_;
    // std::string google_api_key_;
    std::vector<uint8_t> audio_buffer_;
    std::mutex audio_buffer_mutex_;
    
    // 百度云配置
    std::string baidu_api_key_;
    std::string baidu_secret_key_;
    std::string baidu_stt_url_;
    std::string baidu_tts_url_;
    
    // Google配置
    std::string google_api_key_;
    std::string google_llm_url_;

    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
    
    // 百度云相关方法
    bool RefreshBaiduAccessToken();
    std::string BaiduSTT(const std::vector<uint8_t>& audio_data);
    bool BaiduTTS(const std::string& text, std::vector<uint8_t>& audio_data);
    
    // Google相关方法
    std::string GoogleLLM(const std::string& prompt);
    
    // 工具方法
    std::string HttpPost(const std::string& url, const std::string& data, const std::string& content_type);
    std::string HttpGet(const std::string& url);
    std::string GenerateSessionId();
};

#endif