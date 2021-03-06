// WiFiFanCoilMqttMng.ino
// Company: KMP Electronics Ltd, Bulgaria
// Web: http://kmpelectronics.eu/
// Supported boards:
//    KMP ProDino WiFi-ESP WROOM-02 (http://www.kmpelectronics.eu/en-us/products/prodinowifi-esp.aspx)
// Description:
// Example link: 
// Prerequisites: 
//    You should install following libraries:
//		Install with Library Manager. "ArduinoJson by Benoit Blanchon" https://github.com/bblanchon/ArduinoJson
// Version: 0.0.1
// Date: 13.09.2017
// Author: Plamen Kovandjiev <p.kovandiev@kmpelectronics.eu>
// Attention: The project has not finished yet!

#include "FanCoilHelper.h"
#include <KMPDinoWiFiESP.h>       // Our library. https://www.kmpelectronics.eu/en-us/examples/prodinowifi-esp/howtoinstall.aspx
#include "KMPCommon.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>         // Install with Library Manager. "PubSubClient by Nick O'Leary" https://pubsubclient.knolleary.net/
#include <DHT.h>                  // Install with Library Manager. "DHT sensor library by Adafruit" https://github.com/adafruit/DHT-sensor-library
#include <WiFiManager.h>          // Install with Library Manager. "WiFiManager by tzapu" https://github.com/tzapu/WiFiManager
#include <DallasTemperature.h>    // Install with Library Manager. "DallasTemperature by Miles Burton, ..." https://github.com/milesburton/Arduino-Temperature-Control-Library
#include <OneWire.h>			  // This library Installed together with a DallasTemperature library.

DeviceSettings _settings;

WiFiClient _wifiClient;
PubSubClient _mqttClient;
DHT _dhtSensor(DHT_SENSORS_PIN, DHT_SENSORS_TYPE, 11);

OneWire _oneWire(ONEWIRE_SENSORS_PIN);
DallasTemperature _oneWireSensors(&_oneWire);

// Text buffers for topic and payload.
char _topicBuff[128];
char _payloadBuff[32];

float _desiredTemperature = 22.0;
SensorData _temperatureData;
float _tempCollection[TEMPERATURE_ARRAY_LEN];

SensorData _humidityData;
float _humidityCollection[HUMIDITY_ARRAY_LEN];

SensorData _inletData;
float _inletCollection[INLET_ARRAY_LEN];

uint8_t _fanDegree = 0;
Mode _mode = Cold;
DeviceState _deviceState = Off;
DeviceState _lastDeviceState = Off;
unsigned long _checkPingInterval;
bool _isConnected = false;
bool _isStarted = false;
bool _isDHTExists = true;
bool _isDS18b20Exists = true;

void initializeSensorData()
{
	_temperatureData.DataCollection = _tempCollection;
	_temperatureData.DataCollectionLen = TEMPERATURE_ARRAY_LEN;
	_temperatureData.Precision = TEMPERATURE_PRECISION;
	_temperatureData.CheckDataIntervalMS = CHECK_TEMP_INTERVAL_MS;
	_temperatureData.DataType = Temperature;

	_humidityData.DataCollection = _humidityCollection;
	_humidityData.DataCollectionLen = HUMIDITY_ARRAY_LEN;
	_humidityData.Precision = HUMIDITY_PRECISION;
	_humidityData.CheckDataIntervalMS = CHECK_HUMIDITY_INTERVAL_MS;
	_humidityData.DataType = Humidity;

	_inletData.DataCollection = _inletCollection;
	_inletData.DataCollectionLen = INLET_ARRAY_LEN;
	_inletData.Precision = INLET_PRECISION;
	_inletData.CheckDataIntervalMS = CHECK_INLET_INTERVAL_MS;
	_inletData.DataType = InletPipe;
}

/**
* @brief Callback method. It is fire when has information in subscribed topics.
*
* @return void
*/
void callback(char* topic, byte* payload, unsigned int length) {
#ifdef WIFIFCMM_DEBUG
	printTopicAndPayload("Call back", topic, (char*)payload, length);
#endif
	// TODO Check
	size_t baseTopicLen = strlen(_settings.BaseTopic);

	if (!startsWith(topic, _settings.BaseTopic))
	{
		return;
	}

	// Processing base topic - command send all data from device.
	if (strlen(topic) == baseTopicLen && length == 0)
	{
		DeviceData deviceData = (DeviceData)(Temperature | DesiredTemp | FanDegree | CurrentMode | CurrentDeviceState | Humidity | InletPipe);
		publishData(deviceData, true);
		return;
	}

	// Remove prefix basetopic/
	removeStart(topic, baseTopicLen + 1);

	// All other topics finished with /set
	strConcatenate(_topicBuff, 2, TOPIC_SEPARATOR, TOPIC_SET);

	if (!endsWith(topic, _topicBuff))
	{
		return;
	}

	// Remove /set
	removeEnd(topic, strlen(_topicBuff));

	// Processing topic basetopic/mode/set: heat/cold
	if (isEqual(topic, TOPIC_MODE))
	{
		if (isEqual((char*)payload, PAYLOAD_HEAT, length))
		{
			_mode = Heat;
		}

		if (isEqual((char*)payload, PAYLOAD_COLD, length))
		{
			_mode = Cold;
		}

		publishData(CurrentMode);
		//SaveConfiguration();
		return;
	}

	// Processing topic basetopic/desiredtemp/set: 22.5
	if (isEqual(topic, TOPIC_DESIRED_TEMPERATURE))
	{
		memcpy(_topicBuff, payload, length);
		_topicBuff[length] = CH_NONE;

		float temp = atof(_topicBuff);
		float roundTemp = roundF(temp, TEMPERATURE_PRECISION);
		if (roundTemp >= MIN_DESIRED_TEMPERATURE && roundTemp <= MAX_DESIRED_TEMPERATURE)
		{
			_desiredTemperature = roundTemp;
		}

		publishData(DesiredTemp);
		return;
	}

	// Processing topic basetopic/state/set: on, off
	if (isEqual(topic, TOPIC_DEVICE_STATE))
	{
		if (isEqual((char*)payload, PAYLOAD_ON, length))
		{
			_deviceState = _lastDeviceState = On;
		}

		if (isEqual((char*)payload, PAYLOAD_OFF, length))
		{
			_deviceState = _lastDeviceState = Off;
		}

		publishData(CurrentDeviceState);
		//SaveConfiguration();
		return;
	}
}

/**
* @brief Execute first after start the device. Initialize hardware.
*
* @return void
*/
void setup(void)
{
	initializeSensorData();

	// You can open the Arduino IDE Serial Monitor window to see what the code is doing
	DEBUG_FC.begin(115200);
	// Init KMP ProDino WiFi-ESP board.
	KMPDinoWiFiESP.init();
	KMPDinoWiFiESP.SetAllRelaysOff();

	DEBUG_FC_PRINTLN(F("KMP fan coil management with Mqtt.\r\n"));

	//WiFiManager
	//Local initialization. Once it's business is done, there is no need to keep it around.
	WiFiManager wifiManager;

	// Is OptoIn 4 is On the board is resetting WiFi configuration.
	if (KMPDinoWiFiESP.GetOptoInState(OptoIn4))
	{
		DEBUG_FC_PRINTLN(F("Resetting WiFi configuration...\r\n"));
		wifiManager.resetSettings();
		DEBUG_FC_PRINTLN(F("WiFi configuration was reseted.\r\n"));
	}

	// Set save configuration callback.
	wifiManager.setSaveConfigCallback(saveConfigCallback);
	
	if (!mangeConnectParamers(&wifiManager, &_settings))
	{
		return;
	}

	_dhtSensor.begin();
	_oneWireSensors.begin();

	// Initialize MQTT.
	_mqttClient.setClient(_wifiClient);
	uint16_t port = atoi(_settings.MqttPort);
	_mqttClient.setServer(_settings.MqttServer, port);
	_mqttClient.setCallback(callback);
}

/**
* @brief Main method.
*
* @return void
*/
void loop(void)
{
	// For a normal work on device, need it be connected to WiFi and MQTT server.
	_isConnected = connectWiFi() && connectMqtt();

	if (!_isConnected)
	{
		switchOffDevice();
		return;
	}
	else
	{
		_mqttClient.loop();
	}

	bool isDHTExists = getTemperatureAndHumidity();
	processDHTStatus(isDHTExists);

	bool isDS18B20Exists = getPipesTemperature();
	processDS18B20Status(isDS18B20Exists);

	if (!_isStarted)
	{
		setArrayValues(&_temperatureData);
		setArrayValues(&_humidityData);
		setArrayValues(&_inletData);
	}

	{
		processData(&_temperatureData);
		processData(&_humidityData);

		//if (getPipesTemperature())
		//{
		//	processData(&_inletData);
		//} 
	}

	fanCoilControl();

	if (millis() > _checkPingInterval)
	{
		publishData(DevicePing);
	}
}

void switchOffDevice()
{
	processDeviceState(Off);
	_isStarted = false;
}

bool getTemperatureAndHumidity()
{
	_temperatureData.Current = _dhtSensor.readTemperature();
	_humidityData.Current = _dhtSensor.readHumidity();

	bool result = _temperatureData.Current != NAN && _humidityData.Current != NAN;
	
	_temperatureData.IsExists = result;
	_humidityData.IsExists = result;
}

bool getPipesTemperature()
{
	if (!_inletData.IsExists)
	{
		findPipeSensors();
	}

	if (_inletData.IsExists)
	{
		_inletData.Current = _oneWireSensors.getTempC(_inletData.Address);

		_inletData.IsExists = _inletData.Current != NAN;
	}

	return _inletData.IsExists;
}

/**
* @brief 
*/
void processDeviceState(DeviceState state)
{
	if (_deviceState == state)
	{
		return;
	}

	_deviceState = state;

	publishData(CurrentDeviceState);
}

void processDHTStatus(bool isExists)
{
	if (isExists)
	{
		_isDHTExists = true;
		if (_deviceState != _lastDeviceState)
		{
			processDeviceState(_lastDeviceState);
		}
	}
	else
	{
		if (_isDHTExists)
		{
			// Maybe send error
			_isDHTExists = false;
			_lastDeviceState = _deviceState;
			switchOffDevice();
		}
	}
}

void processDS18B20Status(bool isExists)
{
	if (isExists)
	{
		_isDS18b20Exists = true;
	}
	else
	{
		if (_isDS18b20Exists)
		{
			// Maybe send warning
			_isDS18b20Exists = false;
		}
	}
}

//void processConnectionStatus(bool isConnected)
//{
//	if (isConnected)
//	{
//		if (!_isConnected)
//		{
//			_isConnected = true;
//			publishData((DeviceData)(Temperature | DesiredTemp | InletPipe | FanDegree | CurrentMode | CurrentDeviceState | Humidity | DeviceIsReady));
//		}
//		
//		return;
//	}
//
//	_isConnected = false;
//
//	if (_deviceState == On)
//	{
//		_deviceState = Off;
//
//		// Switch off fan.
//		setFanDegree(0);
//	}
//}

//void processData(SensorData* data)
//{
//	if (millis() > data->CheckInterval)
//	{
//		if (data->CurrentCollectPos >= data->DataCollectionLen)
//		{
//			data->CurrentCollectPos = 0;
//		}
//
//		if (_isSensorExist)
//		{
//			data->DataCollection[data->CurrentCollectPos++] = roundF(data->Current, data->Precision);
//		}
//
//		// Process collected data
//		float result = calcAverage(data->DataCollection, data->DataCollectionLen, data->Precision);
//
//		if (data->Average != result)
//		{
//			data->Average = result;
//			publishData(data->DataType);
//		}
//
//		// Set next time to read data.
//		data->CheckInterval = millis() + data->CheckDataIntervalMS;
//	}
//}

void fanCoilControl()
{
	if (_deviceState == Off)
	{
		if (_fanDegree != 0)
		{
			setFanDegree(0);
		}

		return;
	}

	float diffTemp = _mode == Cold ? _temperatureData.Average - _desiredTemperature /* Cold */ : _desiredTemperature - _temperatureData.Average /* Heat */;

	// Bypass the fan coil.
	if (diffTemp < -1.0)
	{
		// TODO: Add bypass logic.
	}

	float pipeDiffTemp = NAN;
	
	if (_inletData.IsExists)
	{
		pipeDiffTemp = _mode == Cold ? _temperatureData.Average - _inletData.Average /* Cold */ : _inletData.Average - _temperatureData.Average /* Heat */;
	}

	uint8_t degree = 0;
	// If inlet sensor doesn't exist or difference between inlet pipe temperature and ambient temperature > 5 degree get fan degree.
	if (pipeDiffTemp == NAN || pipeDiffTemp > 5)
	{
		for (uint8_t i = 0; i < FAN_SWITCH_LEVEL_LEN; i++)
		{
			if (diffTemp <= FAN_SWITCH_LEVEL[i])
			{
				degree = i;
				break;
			}
		}
	}

	setFanDegree(degree);
}

/*
* A fan degree: 0 - stopped, 1 - low fan speed, 2 - medium, 3 - high
*/
void setFanDegree(uint degree)
{
	if (degree == _fanDegree)
	{
		return;
	}

	// Switch last fan degree relay off.
	if (_fanDegree != 0)
	{
		KMPDinoWiFiESP.SetRelayState(_fanDegree - 1, false);
	}

	delay(100);

	if (degree > 0)
	{
		KMPDinoWiFiESP.SetRelayState(degree - 1, true);
	}

	_fanDegree = degree;

	publishData(FanDegree);
}

char* valueToStr(SensorData* sensorData, bool sendCurrent)
{
	if (!sensorData->IsExists)
	{
		return (char*)NOT_AVILABLE;
	}

	float val = sendCurrent ? sensorData->Current : sensorData->Average;

	FloatToChars(val, sensorData->Precision, _payloadBuff);
	return _payloadBuff;
}

/**
* @brief Publish topic.
* @param topic A topic title.
* @param payload A data to send.
*
* @return void
*/
void mqttPublish(const char* topic, char* payload)
{
#ifdef WIFIFCMM_DEBUG
	printTopicAndPayload("Publish", topic, payload, strlen(payload));
#endif
	_mqttClient.publish(topic, (const char*)payload);
}

/**
* @brief Connect to MQTT server.
*
* @return bool true - success.
*/
bool connectMqtt()
{
	if (!_mqttClient.connected())
	{
		DEBUG_FC_PRINTLN(F("Attempting MQTT connection..."));

		if (_mqttClient.connect(_settings.MqttClientId, _settings.MqttUser, _settings.MqttPass))
		{
			DEBUG_FC_PRINTLN(F("MQTT connected. Subscribe for topics:"));
			// Subscribe for topics:
			//  basetopic
			_mqttClient.subscribe(_settings.BaseTopic);
			DEBUG_FC_PRINTLN(_settings.BaseTopic);

			//  basetopic/+/set. This pattern include:  basetopic/mode/set, basetopic/desiredtemp/set, basetopic/state/set
			strConcatenate(_topicBuff, 5, _settings.BaseTopic, TOPIC_SEPARATOR, EVERY_ONE_LEVEL_TOPIC, TOPIC_SEPARATOR, TOPIC_SET);
			_mqttClient.subscribe(_topicBuff);
			DEBUG_FC_PRINTLN(_topicBuff);
		}
		else
		{
			DEBUG_FC_PRINT(F("failed, rc="));
			DEBUG_FC_PRINT(_mqttClient.state());
			DEBUG_FC_PRINTLN(F(" try again after 5 seconds"));
			// Wait 5 seconds before retrying
			delay(5000);
		}
	}

	return _mqttClient.connected();
}

void publishData(DeviceData deviceData, bool sendCurrent = false)
{
	_checkPingInterval = millis() + PING_INTERVAL_MS;

	if (!_isConnected)
	{
		return;
	}

	if (CHECK_ENUM(deviceData, Temperature))
	{
		strConcatenate(_topicBuff, 3, _settings.BaseTopic, TOPIC_SEPARATOR, TOPIC_TEMPERATURE);

		mqttPublish(_topicBuff, valueToStr(&_temperatureData, sendCurrent));
	}

	if (CHECK_ENUM(deviceData, Humidity))
	{
		strConcatenate(_topicBuff, 3, _settings.BaseTopic, TOPIC_SEPARATOR, TOPIC_HUMIDITY);

		mqttPublish(_topicBuff, valueToStr(&_humidityData, sendCurrent));
	}

	if (CHECK_ENUM(deviceData, InletPipe))
	{
		strConcatenate(_topicBuff, 3, _settings.BaseTopic, TOPIC_SEPARATOR, TOPIC_INLET_TEMPERATURE);

		mqttPublish(_topicBuff, valueToStr(&_inletData, sendCurrent));
	}

	if (CHECK_ENUM(deviceData, FanDegree))
	{
		strConcatenate(_topicBuff, 3, _settings.BaseTopic, TOPIC_SEPARATOR, TOPIC_FAN_DEGREE);
		IntToChars(_fanDegree, _payloadBuff);

		mqttPublish(_topicBuff, _payloadBuff);
	}

	if (CHECK_ENUM(deviceData, DesiredTemp))
	{
		strConcatenate(_topicBuff, 3, _settings.BaseTopic, TOPIC_SEPARATOR, TOPIC_DESIRED_TEMPERATURE);
		FloatToChars(_desiredTemperature, TEMPERATURE_PRECISION, _payloadBuff);

		mqttPublish(_topicBuff, _payloadBuff);
	}

	if (CHECK_ENUM(deviceData, CurrentMode))
	{
		strConcatenate(_topicBuff, 3, _settings.BaseTopic, TOPIC_SEPARATOR, TOPIC_MODE);

		const char * mode = _mode == Cold ? PAYLOAD_COLD : PAYLOAD_HEAT;

		mqttPublish(_topicBuff, (char*)mode);
	}

	if (CHECK_ENUM(deviceData, CurrentDeviceState))
	{
		strConcatenate(_topicBuff, 3, _settings.BaseTopic, TOPIC_SEPARATOR, TOPIC_DEVICE_STATE);

		const char * mode = _deviceState == On ? PAYLOAD_ON : PAYLOAD_OFF;

		mqttPublish(_topicBuff, (char*)mode);
	}

	if (CHECK_ENUM(deviceData, DeviceIsReady))
	{
		mqttPublish(_settings.BaseTopic, (char*)PAYLOAD_READY);
	}

	if (CHECK_ENUM(deviceData, DevicePing))
	{
		mqttPublish(_settings.BaseTopic, (char*)PAYLOAD_PING);
	}
}

void findPipeSensors()
{
	uint8_t pipeSensorCount = _oneWireSensors.getDeviceCount();
	if (pipeSensorCount > 0)
	{
		DeviceAddress deviceAddress;

		for (uint8_t i = 0; i < pipeSensorCount; i++)
		{
			bool isExists = _oneWireSensors.getAddress(_inletData.Address, i);
			if (i == 0)
			{
				// Process only first device
				_inletData.IsExists = isExists;
			}

			if (isExists)
			{
				_oneWireSensors.setResolution(deviceAddress, ONEWIRE_TEMPERATURE_PRECISION);
			}
		}
	}
}
