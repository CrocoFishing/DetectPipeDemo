#include <algorithm>
#include <cstring>
#include "application_protocol.hpp"
#include "ble_transport.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace { demo::MetricsService metrics(5); demo::BleTransport ble(metrics); QueueHandle_t requests;
struct Request { uint32_t id; uint32_t bytes; };
void control(const uint8_t* d,size_t n,void*) { protocol::PacketHeader h{}; const uint8_t* p{}; if(protocol::decode_packet(d,n,h,p)!=protocol::DecodeStatus::Ok||h.message_type!=protocol::MessageType::StartCapture)return;
    Request r{h.request_id,std::clamp<uint32_t>(h.reserved? h.reserved:20000,20000,100000)}; xQueueSend(requests,&r,0); }
void result(const uint8_t*,size_t,void*) {}
void tx_task(void*) { uint8_t* data=static_cast<uint8_t*>(heap_caps_malloc(100000,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)); if(!data)vTaskDelete(nullptr);
    for(size_t i=0;i<100000;++i)data[i]=static_cast<uint8_t>((i*31U+17U)&0xffU);
    while(true){Request r{};if(xQueueReceive(requests,&r,portMAX_DELAY)==pdTRUE){ble.send_event(protocol::MessageType::CaptureAccepted,r.id,0);esp_err_t e=ble.send_image(r.id,r.id,data,r.bytes);
        ESP_LOGI("phase05","[TEST] phase=5 request=%lu bytes=%lu result=%s",static_cast<unsigned long>(r.id),static_cast<unsigned long>(r.bytes),esp_err_to_name(e));}}}
}
extern "C" void app_main(){requests=xQueueCreate(2,sizeof(Request));if(!requests||ble.initialize(control,result,nullptr)!=ESP_OK){ESP_LOGE("phase05","init failed");return;}xTaskCreate(tx_task,"ble_tx_task",4096,nullptr,5,nullptr);}

