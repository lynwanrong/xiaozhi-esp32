#include "baidu_google_protocol.h"
#include "board.h"
#include "system_info.h"
#include "settings.h"
#include "application.h"
#include "assets/lang_config.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_http_client.h>
#include <esp_random.h>

#ifdef CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif

#define TAG "BaiduGoogleProtocol"

BaiduGoogleProtocol::BaiduGoogleProtocol() {
    is_audio_channel_opened_ = false;
    
    // 从配置中读取API密钥
    // Settings settings("baidu_google", false);
    // baidu_api_key_ = settings.GetString("baidu_api_key");
    // baidu_secret_key_ = settings.GetString("baidu_secret_key");
    // google_api_key_ = settings.GetString("google_api_key");

    baidu_api_key_ = "N39O3mRNQdUJF1Hpeil9NJVY";
    baidu_secret_key_ = "dpVhEBUejtg9GA607PbReMAPGToUZJy1";
    google_api_key_ = "";
    
    // 设置API URL
    baidu_stt_url_ = "https://vop.baidu.com/server_api";
    baidu_tts_url_ = "https://tsn.baidu.com/text2audio";
    google_llm_url_ = "https://generativelanguage.googleapis.com/v1beta/models/gemini-pro:generateContent";
    
    // 刷新百度访问令牌
    RefreshBaiduAccessToken();
}

BaiduGoogleProtocol::~BaiduGoogleProtocol() {
}

bool BaiduGoogleProtocol::Start() {
    // 检查必要配置是否存在
    if (baidu_api_key_.empty() || baidu_secret_key_.empty() || google_api_key_.empty()) {
        ESP_LOGE(TAG, "API keys not configured");
        return false;
    }
    
    return true;
}

bool BaiduGoogleProtocol::OpenAudioChannel() {
    session_id_ = GenerateSessionId();
    is_audio_channel_opened_ = true;
    
    if (on_audio_channel_opened_) {
        on_audio_channel_opened_();
    }
    
    return true;
}

void BaiduGoogleProtocol::CloseAudioChannel() {
    is_audio_channel_opened_ = false;
    audio_buffer_.clear();
    
    if (on_audio_channel_closed_) {
        on_audio_channel_closed_();
    }
}

bool BaiduGoogleProtocol::IsAudioChannelOpened() const {
    return is_audio_channel_opened_;
}

bool BaiduGoogleProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    // 将音频数据累积到缓冲区
    {
        std::lock_guard<std::mutex> lock(audio_buffer_mutex_);
        audio_buffer_.insert(audio_buffer_.end(), packet->payload.begin(), packet->payload.end());
    }
    
    // 当缓冲区足够大时，发送到百度进行STT
    if (audio_buffer_.size() > 16000) { // 例如超过1秒的音频数据
        std::vector<uint8_t> audio_data;
        {
            std::lock_guard<std::mutex> lock(audio_buffer_mutex_);
            audio_data = std::move(audio_buffer_);
            audio_buffer_.clear();
        }
        
        // 调用百度STT
        std::string stt_result = BaiduSTT(audio_data);
        if (!stt_result.empty()) {
            // 构造STT结果JSON
            cJSON* root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "type", "stt");
            cJSON_AddStringToObject(root, "text", stt_result.c_str());
            
            char* json_str = cJSON_PrintUnformatted(root);
            if (on_incoming_json_ && json_str) {
                cJSON* parsed = cJSON_Parse(json_str);
                if (parsed) {
                    on_incoming_json_(parsed);
                    cJSON_Delete(parsed);
                }
            }
            
            if (json_str) {
                cJSON_free(json_str);
            }
            cJSON_Delete(root);
            
            // 调用Google LLM
            std::string llm_response = GoogleLLM(stt_result);
            if (!llm_response.empty()) {
                // 构造LLM结果JSON
                cJSON* llm_root = cJSON_CreateObject();
                cJSON_AddStringToObject(llm_root, "type", "llm");
                cJSON_AddStringToObject(llm_root, "emotion", "neutral");
                
                cJSON* content = cJSON_CreateObject();
                cJSON_AddStringToObject(content, "text", llm_response.c_str());
                cJSON_AddItemToObject(llm_root, "content", content);
                
                char* llm_json_str = cJSON_PrintUnformatted(llm_root);
                if (on_incoming_json_ && llm_json_str) {
                    cJSON* parsed = cJSON_Parse(llm_json_str);
                    if (parsed) {
                        on_incoming_json_(parsed);
                        cJSON_Delete(parsed);
                    }
                }
                
                if (llm_json_str) {
                    cJSON_free(llm_json_str);
                }
                cJSON_Delete(llm_root);
                
                // 调用百度TTS
                std::vector<uint8_t> tts_audio;
                if (BaiduTTS(llm_response, tts_audio)) {
                    // 构造TTS音频包
                    auto audio_packet = std::make_unique<AudioStreamPacket>();
                    audio_packet->payload = std::move(tts_audio);
                    audio_packet->sample_rate = 16000;
                    audio_packet->frame_duration = 60;
                    
                    if (on_incoming_audio_) {
                        on_incoming_audio_(std::move(audio_packet));
                    }
                }
            }
        }
    }
    
    return true;
}

bool BaiduGoogleProtocol::SendText(const std::string& text) {
    // 处理文本消息（如唤醒词检测等）
    cJSON* root = cJSON_Parse(text.c_str());
    if (!root) {
        return false;
    }
    
    auto type = cJSON_GetObjectItem(root, "type");
    if (type && cJSON_IsString(type)) {
        if (strcmp(type->valuestring, "listen") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (state && cJSON_IsString(state)) {
                if (strcmp(state->valuestring, "detect") == 0) {
                    // 唤醒词检测到，可以在这里处理
                }
            }
        }
    }
    
    cJSON_Delete(root);
    return true;
}

std::string BaiduGoogleProtocol::GetHelloMessage() {
    return "{}";
}

bool BaiduGoogleProtocol::RefreshBaiduAccessToken() {
    std::string url = "https://aip.baidubce.com/oauth/2.0/token";
    std::string post_data = "grant_type=client_credentials&client_id=" + baidu_api_key_ + 
                           "&client_secret=" + baidu_secret_key_;
    
    std::string response = HttpPost(url, post_data, "application/x-www-form-urlencoded");
    if (response.empty()) {
        return false;
    }
    
    cJSON* root = cJSON_Parse(response.c_str());
    if (!root) {
        return false;
    }
    
    auto access_token = cJSON_GetObjectItem(root, "access_token");
    if (access_token && cJSON_IsString(access_token)) {
        baidu_access_token_ = access_token->valuestring;
        ESP_LOGI(TAG, "Baidu Access Token: %s", baidu_access_token_.c_str());
        cJSON_Delete(root);
        return true;
    }
    
    cJSON_Delete(root);
    return false;
}

std::string BaiduGoogleProtocol::BaiduSTT(const std::vector<uint8_t>& audio_data) {
    if (baidu_access_token_.empty()) {
        if (!RefreshBaiduAccessToken()) {
            return "";
        }
    }
    
    std::string url = baidu_stt_url_ + "?dev_pid=1537&token=" + baidu_access_token_;
    
    // 构造JSON请求
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "format", "opus");
    cJSON_AddNumberToObject(root, "rate", 16000);
    cJSON_AddNumberToObject(root, "channel", 1);
    cJSON_AddStringToObject(root, "cuid", "xiaozhi_device");
    cJSON_AddStringToObject(root, "token", baidu_access_token_.c_str());
    cJSON_AddNumberToObject(root, "len", audio_data.size());
    
    // Base64编码音频数据
    std::string base64_audio = ""; // 实际应用中需要实现Base64编码
    cJSON_AddStringToObject(root, "speech", base64_audio.c_str());
    
    char* json_str = cJSON_PrintUnformatted(root);
    std::string json_string(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    std::string response = HttpPost(url, json_string, "application/json");
    if (response.empty()) {
        return "";
    }
    
    cJSON* response_root = cJSON_Parse(response.c_str());
    if (!response_root) {
        return "";
    }
    
    auto result = cJSON_GetObjectItem(response_root, "result");
    if (result && cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
        auto first_result = cJSON_GetArrayItem(result, 0);
        if (first_result && cJSON_IsString(first_result)) {
            std::string text = first_result->valuestring;
            cJSON_Delete(response_root);
            return text;
        }
    }
    
    cJSON_Delete(response_root);
    return "";
}

bool BaiduGoogleProtocol::BaiduTTS(const std::string& text, std::vector<uint8_t>& audio_data) {
    if (baidu_access_token_.empty()) {
        if (!RefreshBaiduAccessToken()) {
            return false;
        }
    }
    
    std::string url = baidu_tts_url_;
    std::string post_data = "tex=" + text + 
                           "&tok=" + baidu_access_token_ + 
                           "&cuid=xiaozhi_device&ctp=1&lan=zh&spd=5&pit=5&vol=5&per=0";
    
    // 注意：百度TTS返回的是音频数据，不是JSON
    // 这里需要特殊处理HTTP响应
    // 为简化起见，这里仅示意
    
    return true;
}

std::string BaiduGoogleProtocol::GoogleLLM(const std::string& prompt) {
    std::string url = google_llm_url_ + "?key=" + google_api_key_;
    
    // 构造Google Gemini请求
    cJSON* root = cJSON_CreateObject();
    
    cJSON* contents = cJSON_CreateArray();
    cJSON_AddItemToArray(contents, cJSON_CreateObject());
    
    cJSON* parts = cJSON_CreateArray();
    cJSON* part = cJSON_CreateObject();
    cJSON_AddStringToObject(part, "text", prompt.c_str());
    cJSON_AddItemToArray(parts, part);
    
    cJSON* content = cJSON_GetArrayItem(contents, 0);
    cJSON_AddItemToObject(content, "parts", parts);
    
    cJSON_AddItemToObject(root, "contents", contents);
    
    char* json_str = cJSON_PrintUnformatted(root);
    std::string json_string(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    
    std::string response = HttpPost(url, json_string, "application/json");
    if (response.empty()) {
        return "";
    }
    
    cJSON* response_root = cJSON_Parse(response.c_str());
    if (!response_root) {
        return "";
    }
    
    // 解析Google Gemini响应
    auto candidates = cJSON_GetObjectItem(response_root, "candidates");
    if (candidates && cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) > 0) {
        auto first_candidate = cJSON_GetArrayItem(candidates, 0);
        auto content = cJSON_GetObjectItem(first_candidate, "content");
        auto parts = cJSON_GetObjectItem(content, "parts");
        if (parts && cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0) {
            auto first_part = cJSON_GetArrayItem(parts, 0);
            auto text = cJSON_GetObjectItem(first_part, "text");
            if (text && cJSON_IsString(text)) {
                std::string result = text->valuestring;
                cJSON_Delete(response_root);
                return result;
            }
        }
    }
    
    cJSON_Delete(response_root);
    return "";
}


std::string BaiduGoogleProtocol::HttpPost(const std::string& url, const std::string& data, const std::string& content_type) {
   esp_http_client_config_t config = {};
   config.url = url.c_str();
   config.method = HTTP_METHOD_POST;
   config.transport_type = HTTP_TRANSPORT_OVER_SSL;
   config.buffer_size = 2048;
   config.skip_cert_common_name_check = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return "";
    }
    
    esp_http_client_set_header(client, "Content-Type", content_type.c_str());
    esp_http_client_set_post_field(client, data.c_str(), data.length());
    
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return "";
    }
    
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP POST request failed with status code: %d", status_code);
        esp_http_client_cleanup(client);
        return "";
    }
    
    int content_length = esp_http_client_get_content_length(client);
    if (content_length <= 0) {
        esp_http_client_cleanup(client);
        return "";
    }
    
    std::string response(content_length, 0);
    int read_len = esp_http_client_read(client, &response[0], content_length);
    if (read_len <= 0) {
        esp_http_client_cleanup(client);
        return "";
    }
    
    response.resize(read_len);
    esp_http_client_cleanup(client);
    return response;
}

std::string BaiduGoogleProtocol::HttpGet(const std::string& url) {
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.method = HTTP_METHOD_GET;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.buffer_size = 2048;
    config.skip_cert_common_name_check = true;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return "";
    }
    
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return "";
    }
    
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP GET request failed with status code: %d", status_code);
        esp_http_client_cleanup(client);
        return "";
    }
    
    int content_length = esp_http_client_get_content_length(client);
    if (content_length <= 0) {
        esp_http_client_cleanup(client);
        return "";
    }
    
    std::string response(content_length, 0);
    int read_len = esp_http_client_read(client, &response[0], content_length);
    if (read_len <= 0) {
        esp_http_client_cleanup(client);
        return "";
    }
    
    response.resize(read_len);
    esp_http_client_cleanup(client);
    return response;
}


std::string BaiduGoogleProtocol::GenerateSessionId() {
    uint32_t random = esp_random();
    char session_id[32];
    snprintf(session_id, sizeof(session_id), "%08lX", random);
    return std::string(session_id);
}