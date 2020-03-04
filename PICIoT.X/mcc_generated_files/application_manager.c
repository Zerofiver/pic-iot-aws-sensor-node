/*
\file   application_manager.c

\brief  Application Manager source file.

(c) 2018 Microchip Technology Inc. and its subsidiaries.

Subject to your compliance with these terms, you may use Microchip software and any
derivatives exclusively with Microchip products. It is your responsibility to comply with third party
license terms applicable to your use of third party software (including open source software) that
may accompany Microchip software.

THIS SOFTWARE IS SUPPLIED BY MICROCHIP "AS IS". NO WARRANTIES, WHETHER
EXPRESS, IMPLIED OR STATUTORY, APPLY TO THIS SOFTWARE, INCLUDING ANY
IMPLIED WARRANTIES OF NON-INFRINGEMENT, MERCHANTABILITY, AND FITNESS
FOR A PARTICULAR PURPOSE.

IN NO EVENT WILL MICROCHIP BE LIABLE FOR ANY INDIRECT, SPECIAL, PUNITIVE,
INCIDENTAL OR CONSEQUENTIAL LOSS, DAMAGE, COST OR EXPENSE OF ANY KIND
WHATSOEVER RELATED TO THE SOFTWARE, HOWEVER CAUSED, EVEN IF MICROCHIP
HAS BEEN ADVISED OF THE POSSIBILITY OR THE DAMAGES ARE FORESEEABLE. TO
THE FULLEST EXTENT ALLOWED BY LAW, MICROCHIP'S TOTAL LIABILITY ON ALL
CLAIMS IN ANY WAY RELATED TO THIS SOFTWARE WILL NOT EXCEED THE AMOUNT
OF FEES, IF ANY, THAT YOU HAVE PAID DIRECTLY TO MICROCHIP FOR THIS
SOFTWARE.
*/

#include <string.h>
#include <time.h>
#include <stdio.h>
#include "pin_manager.h"
#include "timeout.h"
#include "application_manager.h"
#include "mcc.h"
#include "config/IoT_Sensor_Node_config.h"
#include "config/conf_winc.h"
#include "config/mqtt_config.h"
#include "cloud/cloud_service.h"
#include "cloud/mqtt_service.h"
#include "cloud/crypto_client/crypto_client.h"
#include "cloud/wifi_service.h"
#include "CryptoAuthenticationLibrary/CryptoAuth_init.h"
#include "../mcc_generated_files/sensors_handling.h"
#include "credentials_storage/credentials_storage.h"
#include "led.h"
#include "debug_print.h"
#include "time_service.h"
#if CFG_ENABLE_CLI
#include "cli/cli.h"
#endif


#define MAIN_DATATASK_INTERVAL 100L
// The debounce time is currently close to 2 Seconds.
#define SW_DEBOUNCE_INTERVAL   1460000L
#define SW0_TOGGLE_STATE	   SW0_GetValue()
#define SW1_TOGGLE_STATE	   SW1_GetValue()
#define TOGGLE_ON  1
#define TOGGLE_OFF 0
#define DEVICE_SHADOW_INIT_INTERVAL 1000L
#define UPDATE_DEVICE_SHADOW_BUFFER_TIME (2)
static uint8_t toggleState = 0;

// This will contain the device ID, before we have it this dummy value is the init value which is non-0
char attDeviceID[20] = "BAAAAADD1DBAAADD1D";
char mqttSubscribeTopic[SUBSCRIBE_TOPIC_SIZE];
ATCA_STATUS retValCryptoClientSerialNumber;
static uint8_t holdCount = 0;

uint32_t MAIN_dataTask(void *payload);
timerStruct_t MAIN_dataTasksTimer = {MAIN_dataTask};

static void  wifiConnectionStateChanged(uint8_t status);
static void sendToCloud(void);
static void updateDeviceShadow(void);
static void subscribeToCloud(void);
static void receivedFromCloud(uint8_t *topic, uint8_t *payload);
static void setToggleState(uint8_t passedToggleState);
static uint8_t getToggleState(void);
uint32_t initDeviceShadow(void *payload);
timerStruct_t initDeviceShadowTimer = {initDeviceShadow};

// This will get called every 1 second only while we have a valid Cloud connection
static void sendToCloud(void)
{
    static char json[PAYLOAD_SIZE];
    static char publishMqttTopic[PUBLISH_TOPIC_SIZE];
    ledTickState_t ledState;
    int rawTemperature = 0;
    int light = 0;
    int len = 0;    
    memset((void*)publishMqttTopic, 0, sizeof(publishMqttTopic));
    sprintf(publishMqttTopic, "%s/sensors", cid);
    // This part runs every CFG_SEND_INTERVAL seconds
    if (shared_networking_params.haveAPConnection)
    {
        rawTemperature = SENSORS_getTempValue();
        light = SENSORS_getLightValue();
        len = sprintf(json,"{\"Light\":%d,\"Temp\":%d.%02d}", light,rawTemperature/100,abs(rawTemperature)%100);
    }
    if (len >0) 
    {
        CLOUD_publishData((uint8_t*)publishMqttTopic ,(uint8_t*)json, len);        
        if (holdCount)
        {
            holdCount--;
        }
        else
        {
            ledState.Full2Sec = LED_BLIP;
            LED_modeYellow(ledState);
        }
        
    }
}

//This handles messages published from the MQTT server when subscribed
static void receivedFromCloud(uint8_t *topic, uint8_t *payload)
{
    char *toggleToken = "\"toggle\":";
    char *subString;
    ledTickState_t ledState;
   sprintf(mqttSubscribeTopic, "$aws/things/%s/shadow/update/delta", cid);
    if (strncmp((void*) mqttSubscribeTopic, (void*) topic, strlen(mqttSubscribeTopic)) == 0) 
    {
        if ((subString = strstr((char*)payload, toggleToken)))
        {
            if (subString[strlen(toggleToken)] == '1')
            {   
                setToggleState(TOGGLE_ON);
                ledState.Full2Sec = LED_ON_STATIC;
                LED_modeYellow(ledState);
            }
            else
            {
                setToggleState(TOGGLE_OFF);
                ledState.Full2Sec = LED_OFF_STATIC;
                LED_modeYellow(ledState);
            }
            holdCount = 2;
        }
    }
    debug_printer(SEVERITY_NONE, LEVEL_NORMAL, "topic: %s", topic);
    debug_printer(SEVERITY_NONE, LEVEL_NORMAL, "payload: %s", payload);
    updateDeviceShadow();
}

void application_init()
{
	uint8_t mode = WIFI_DEFAULT;
	uint32_t sw0CurrentVal = 0;
	uint32_t sw1CurrentVal = 0;
	uint32_t i = 0;
    ledTickState_t ledState;

    // Initialization of modules before interrupts are enabled
    SYSTEM_Initialize();

    // Blocking debounce
    timeout_flushAll();
    for(i = 0; i < SW_DEBOUNCE_INTERVAL; i++)
    {
        sw0CurrentVal += SW0_TOGGLE_STATE;
        sw1CurrentVal += SW1_TOGGLE_STATE;
    }

    LED_test();
#if CFG_ENABLE_CLI     
    CLI_init();
    CLI_setdeviceId(attDeviceID);
#endif   
    debug_init(attDeviceID);   
    
    // Initialization of modules where the init needs interrupts to be enabled
    if(!CryptoAuth_Initialize())
    {
        debug_printError("APP: CryptoAuthInit failed");
        shared_networking_params.haveError = 1;
    }
    // Get serial number from the ECC608 chip 
    retValCryptoClientSerialNumber = CRYPTO_CLIENT_printSerialNumber(attDeviceID);
    if( retValCryptoClientSerialNumber != ATCA_SUCCESS )
    {
        shared_networking_params.haveError = 1;
        switch(retValCryptoClientSerialNumber)
        {
            case ATCA_GEN_FAIL:
                debug_printError("APP: DeviceID generation failed, unspecified error");
                break;
            case ATCA_BAD_PARAM:
                debug_printError("APP: DeviceID generation failed, bad argument");
            default:
                debug_printError("APP: DeviceID generation failed");
                break;
        }
        
    }
#if CFG_ENABLE_CLI   
    CLI_setdeviceId(attDeviceID);
#endif   
    debug_setPrefix(attDeviceID);     
    wifi_readThingIdFromWinc();
    wifi_readAWSEndpointFromWinc();
    timeout_create(&initDeviceShadowTimer, DEVICE_SHADOW_INIT_INTERVAL );    
    if(sw0CurrentVal < (SW_DEBOUNCE_INTERVAL/2))
    {
        if(sw1CurrentVal < (SW_DEBOUNCE_INTERVAL/2))
        {    
            // Default Credentials + Connect to AP
            strcpy(ssid, CFG_MAIN_WLAN_SSID);
            strcpy(pass, CFG_MAIN_WLAN_PSK);
            sprintf((char*)authType, "%d", CFG_MAIN_WLAN_AUTH);
            
            ledState.Full2Sec = LED_BLINK;
            LED_modeGreen(ledState);
            LED_modeBlue(ledState);
            ledState.Full2Sec = LED_OFF_STATIC;
            LED_modeYellow(ledState);
            LED_modeRed(ledState);
            
            shared_networking_params.amConnectingAP = 1;
            shared_networking_params.amSoftAP = 0;
            shared_networking_params.amDefaultCred = 1;
        }
        else
        {    
            // Host as SOFT AP
            ledState.Full2Sec = LED_BLIP;
            LED_modeBlue(ledState);
            ledState.Full2Sec = LED_OFF_STATIC;
            LED_modeGreen(ledState);
            LED_modeYellow(ledState);
            LED_modeRed(ledState);
            mode = WIFI_SOFT_AP;
            shared_networking_params.amConnectingAP = 0;
            shared_networking_params.amSoftAP = 1;
            shared_networking_params.amDefaultCred = 0;
        }
    }
    else
    {    
        // Connect to AP
        ledState.Full2Sec = LED_BLINK;
        LED_modeBlue(ledState);
        ledState.Full2Sec = LED_OFF_STATIC;
        LED_modeGreen(ledState);
        LED_modeYellow(ledState);
        LED_modeRed(ledState);
        shared_networking_params.amConnectingAP = 1;
        shared_networking_params.amSoftAP = 0;
        shared_networking_params.amDefaultCred = 0;
    }
    wifi_init(wifiConnectionStateChanged, mode);
    
    if (mode == WIFI_DEFAULT) 
    {
        CLOUD_setupTask(attDeviceID);
        timeout_create(&MAIN_dataTasksTimer, MAIN_DATATASK_INTERVAL);
    }
    
    LED_test();
    LED_serviceInit();
    subscribeToCloud();
}

static void subscribeToCloud(void)
{
    sprintf(mqttSubscribeTopic, "$aws/things/%s/shadow/update/delta", cid); 
    CLOUD_registerSubscription((uint8_t*)mqttSubscribeTopic,receivedFromCloud);
}

static void setToggleState(uint8_t passedToggleState)
{
    toggleState = passedToggleState;
}

static uint8_t getToggleState(void)
{
    return toggleState;
}

uint32_t initDeviceShadow(void *payload)
{
    static uint32_t previousTime = 0;
    if(CLOUD_checkIsConnected())
    {    
       // Get the current time. This uses the C standard library time functions
       uint32_t timeNow = TIME_getCurrent();
       if(previousTime == 0)
       {
           previousTime = timeNow;         
       }
       else if((TIME_getDiffTime(timeNow, previousTime)) >= UPDATE_DEVICE_SHADOW_BUFFER_TIME)
       {
           updateDeviceShadow();
           return 0; 
       }
    }
    return DEVICE_SHADOW_INIT_INTERVAL;
}

static void updateDeviceShadow(void)
{
    static char payload[PAYLOAD_SIZE];
    static char topic[PUBLISH_TOPIC_SIZE];
    int payloadLength = 0;
     
    memset((void*)topic, 0, sizeof(topic));
    sprintf(topic, "$aws/things/%s/shadow/update", cid);
    if (shared_networking_params.haveAPConnection)
    { 
        payloadLength = sprintf(payload,"{\"state\":{\"reported\":{\"toggle\":%d}}}", getToggleState());
    }
    if (payloadLength >0) 
    {
        CLOUD_publishData((uint8_t*)topic,(uint8_t*)payload, payloadLength); 
    }
}

// This scheduler will check all tasks and timers that are due and service them
void runScheduler(void)
{
    timeout_callNextCallback();
}

// This gets called by the scheduler approximately every 100ms
uint32_t MAIN_dataTask(void *payload)
{
    ledTickState_t ledState;
    static uint32_t previousTransmissionTime = 0;
    
    // Get the current time. This uses the C standard library time functions
    uint32_t timeNow = TIME_getCurrent();
    
    // Example of how to send data when MQTT is connected every 1 second based on the system clock
    if(CLOUD_checkIsConnected())
    {
        // How many seconds since the last time this loop ran?
        int32_t delta = TIME_getDiffTime(timeNow, previousTransmissionTime);
        
        if (delta >= CFG_SEND_INTERVAL)
        {
            previousTransmissionTime = timeNow;
            // Call the data task in main.c
            sendToCloud();
        }
    } 
    
    // Blue LED
    if (!shared_networking_params.amConnectingAP)
    {
        if (shared_networking_params.haveAPConnection)
        {
            ledState.Full2Sec = LED_ON_STATIC;
            LED_modeBlue(ledState);
        }
        
        // Green LED if we are in Access Point
        if (!shared_networking_params.amConnectingSocket)
        {
            if(CLOUD_checkIsConnected())
            {
                ledState.Full2Sec = LED_ON_STATIC;
                LED_modeGreen(ledState);
            }
        }
    }
    
    // RED LED
    if (shared_networking_params.haveError)
    {
        ledState.Full2Sec = LED_ON_STATIC;
        LED_modeRed(ledState);
    }
    else
    {
        ledState.Full2Sec = LED_OFF_STATIC;
        LED_modeRed(ledState);
    }
        
    // This is milliseconds managed by the RTC and the scheduler, this return 
    // makes the timer run another time, returning 0 will make it stop
    return MAIN_DATATASK_INTERVAL; 
}

void application_post_provisioning(void)
{
    CLOUD_setupTask(attDeviceID);
    timeout_create(&MAIN_dataTasksTimer, MAIN_DATATASK_INTERVAL);
}

// React to the WIFI state change here. Status of 1 means connected, Status of 0 means disconnected
static void  wifiConnectionStateChanged(uint8_t status)
{
    // If we have no AP access we want to retry
    if (status != 1)
    {
        // Restart the WIFI module if we get disconnected from the WiFi Access Point (AP)
        CLOUD_setInitFlag();
    } 
}
