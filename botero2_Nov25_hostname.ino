//Include libraries
    #include <ESP8266WiFi.h>
    #include <PubSubClient.h>
    #include <SoftwareSerial.h>
    #include <Wire.h>    
    #include <Adafruit_VL53L0X.h>
    #include <ESP8266WebServer.h>
    #include <EEPROM.h>
    #include <ArduinoOTA.h>
    #include <ESP8266HTTPUpdateServer.h>


//MQTT Variables
    #define mqtt_server "192.168.1.252"
    #define mqtt_port 1883
    #define mqtt_topic "micronova"
    #define mqtt_user "mqtt"
    #define mqtt_pass "mqtt"
    #define ssid "domopoui"
    #define password "0645349586"
    
//ESP Pinout configuration
    SoftwareSerial StoveSerial;
    #define SERIAL_MODE SWSERIAL_8N2 //8 data bits, parity none, 2 stop bits
    #define RX_PIN D3
    #define TX_PIN D4
    #define ENABLE_RX D2
    #define SCL D1
    #define SDA D6


//Capteur de distance
Adafruit_VL53L0X lox;
int  hauteur_cuve=500;
ESP8266WebServer webServer(80);
ESP8266HTTPUpdateServer httpUpdater;   // mise a jour OTA via la page web (/update)
bool hauteur_set = false;        // indique si modifiée via web

//Hostname (valeur par defaut + stockage en EEPROM)
    #define DEFAULT_HOSTNAME "micronova"
    #define HOSTNAME_MAX_LEN 32
    #define EEPROM_SIZE 64
    #define EEPROM_MAGIC 0xA5   // marqueur : un hostname valide est stocke en EEPROM
    String espHostname = DEFAULT_HOSTNAME;

//Mise a jour OTA (identifiants a changer !)
    #define ota_user "admin"
    #define ota_pass "ChangeMoi"

//Un hostname valide : 1 a 32 caracteres, lettres/chiffres/tirets, sans tiret au debut ou a la fin
bool isValidHostname(const String &name)
{
    size_t len = name.length();
    if (len < 1 || len > HOSTNAME_MAX_LEN) return false;
    if (name[0] == '-' || name[len - 1] == '-') return false;
    for (size_t i = 0; i < len; i++)
    {
        char c = name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

//Lit le hostname en EEPROM (octet 0 = marqueur, octet 1 = longueur, puis les caracteres)
void loadHostname()
{
    EEPROM.begin(EEPROM_SIZE);
    espHostname = DEFAULT_HOSTNAME;
    if (EEPROM.read(0) == EEPROM_MAGIC)
    {
        uint8_t len = EEPROM.read(1);
        if (len >= 1 && len <= HOSTNAME_MAX_LEN)
        {
            String stored = "";
            for (uint8_t i = 0; i < len; i++) stored += (char)EEPROM.read(2 + i);
            if (isValidHostname(stored)) espHostname = stored;
        }
    }
}

//Enregistre un nouveau hostname en EEPROM
void saveHostname(const String &name)
{
    EEPROM.write(0, EEPROM_MAGIC);
    EEPROM.write(1, name.length());
    for (size_t i = 0; i < name.length(); i++) EEPROM.write(2 + i, name[i]);
    EEPROM.commit();
    espHostname = name;
}

WiFiClient espClient;
PubSubClient client(espClient);

int value = 0;
unsigned long previousMillis = 0;
int count_reboot;
int distance=0;
String consigne="Consigne inconnue";
String temperature="Temperature inconnue";
int measure_sleep=0;
String stove_state="Etat inconnu";
float lastQte = -1; // Variable globale pour mémoriser la dernière valeur publiée


//Mqtt Topic definitions
    #define ambtemp_topic mqtt_topic "/ambtemp"
    #define stovepower_topic mqtt_topic "/stovepower"
    #define blowtemp_topic mqtt_topic "/blowtemp"
    #define state_topic mqtt_topic "/stovestate"
    #define extractor_topic mqtt_topic "/extractor"
    #define blowspeed_topic mqtt_topic "/blowspeed"
    #define fumetemp_topic mqtt_topic "/fumetemp"
    #define vispellet_topic mqtt_topic "/vispellet"
    #define cardtemp_topic mqtt_topic "/cardtemp"
    #define pwrset_topic mqtt_topic "/pwrset"
    #define tempset_topic mqtt_topic "/tempset"
    #define fixvac_topic mqtt_topic "/fixvac"
    #define readvac_topic mqtt_topic "/readvac"
    #define pong_topic mqtt_topic "/pong"
    #define onoff_topic mqtt_topic "/onoff"
    #define cmd_topic mqtt_topic "/cmd"
    #define multvac_topic mqtt_topic "/multvac"
    #define distance_topic mqtt_topic "/distance"
    #define qte_topic mqtt_topic "/qte"
    #define settemp_topic mqtt_topic "/settemp"
    #define log_topic mqtt_topic "/log"
    #define hauteur_topic mqtt_topic "/hauteur"
    #define device_information "{\"manufacturer\": \"Philibert Cheminot\",\"identifiers\": [\"42396f39-80d2-493b-8e8e-31a70e700bc6\"],\"model\": \"Micronova Controller\",\"name\": \"Micronova Controller\",\"sw_version\": \"1.0.0.0\"}"


//State :0 - Arrêt, 1 - Ventilation Initiale, 2 - Préchauffage Resistance, 3 - Préchargement Pellet, 4 - Attente Flamme, 5 - Attente Allumage, 6 - Allumage, 7 - Travail / Chauffage, 8 - Nettoyage creuset, 9 - Nettoyage Final, 10 - Attente Standby, 11 - Standby, 12 - Alarme pellets, 13 - Alarme(to be investigated)
//Checksum: Code+Address+Value on hexadecimal calculator

// RAM address
    #define _RAMR           0x00
    #define _RAMW           0x80
    #define stoveStateAddr	  0x21
    #define ambTempAddr 	  0x01
    #define stovePwrAddr  	  0x03
    #define blowTempAddr  	  0x09
    #define extractorAddr	  0x37
    #define blowSpeedAddr	  0x59
    #define fumeTempAddr	  0x5C
    #define visPelletAddr	  0x63
    #define cardTempAddr	  0x9A
    #define fixVacuumAddr	  0x75
    #define readVacuumAddr	  0x76
    #define multVacuumAddr    0x77


// EEPROM defines
    #define _EEPROMR        0x20
    #define _EEPROMW        0xA0
    #define pwrSetAddr		  0xD0
    #define tempSetAddr		  0x6B

    const char stoveOn[4] = {_RAMW, stoveStateAddr, 0x01, (char)(_RAMW + stoveStateAddr + 0x01)};
    const char stoveStb[4] = {_RAMW, stoveStateAddr, 0x0B, (char)(_RAMW + stoveStateAddr + 0x0B)};
    const char forceOff[4] = {_RAMW, stoveStateAddr, 0x00, (char)(_RAMW + stoveStateAddr + 0x00)};

    const char stovePWA[4]   = {_EEPROMW, pwrSetAddr, 0x00, (char)(_EEPROMW + pwrSetAddr + 0x00)};
    const char stovePW1[4]   = {_EEPROMW, pwrSetAddr, 0x01, (char)(_EEPROMW + pwrSetAddr + 0x01)};
    const char stovePW2[4]   = {_EEPROMW, pwrSetAddr, 0x02, (char)(_EEPROMW + pwrSetAddr + 0x02)};
    const char stovePW3[4]   = {_EEPROMW, pwrSetAddr, 0x03, (char)(_EEPROMW + pwrSetAddr + 0x03)};
    const char stovePW4[4]   = {_EEPROMW, pwrSetAddr, 0x04, (char)(_EEPROMW + pwrSetAddr + 0x04)};
    const char stovePW5[4]   = {_EEPROMW, pwrSetAddr, 0x05, (char)(_EEPROMW + pwrSetAddr + 0x05)};


//set variable
    uint8_t stovePower, stoveState, fumeTemp, blowTemp, pwrSet ;
    float ambTemp, extractor, blowSpeed, visPellet, cardTemp, tempSet, fixVac, readVac, multVac;
    char stoveRxData[2];     //When the heater is sending data, it sends two bytes: a checksum and the value

//publie sur le broker MQTT
    bool mqttPublish(PubSubClient &client, const String &topic, const String &payload, bool retained = true) {
    Serial.print("[MQTT] Publish to ");
    Serial.print(topic);
    Serial.print(" → ");
    Serial.println(payload);

    bool success = client.publish(topic.c_str(), payload.c_str(), retained);

    if (!success) {
        Serial.println("[MQTT] ❌ Publish failed!");
    } else {
        Serial.println("[MQTT] ✅ Publish OK");
    }

    return success;
    }

//void log
    void Write_Log(PubSubClient &client, const String &text) {
        client.publish("micronova/log", text.c_str(), false);
        Serial.println("[LOG] " + text);
    }

//Setup WiFiManager and connect to WiFi
    void setup_wifi() 
    {        
        Serial.println("");
        Serial.println("Wifi : configuration");
        delay(10);
        WiFi.mode(WIFI_STA);
        WiFi.hostname(espHostname);
        WiFi.begin(ssid, password);
        while (WiFi.status() != WL_CONNECTED) {
            delay(500);
        }
        Serial.println("Wifi : connected");
        Serial.print("Wifi : hostname: ");
        Serial.println(espHostname);
        Serial.print("Wifi : IP address: ");
        Serial.println(WiFi.localIP());
        mqttPublish(client, log_topic, WiFi.localIP().toString(), true);
    }



   
//Connect to MQTT server
    void reconnect_mqtt() 
    {
        Write_Log(client, "MQTT : configuration");
        Serial.println();
        client.setServer(mqtt_server, mqtt_port);
        client.setCallback(callback);
        //Loop until we're reconnected
        while (!client.connected())
        {
            
            if (client.connect("ESPClient", mqtt_user, mqtt_pass))
            {
                client.setBufferSize(1024);
            }
            else
            {
                delay(5000);
            }
        }
        if (client.connected())
        {
            Write_Log(client, "MQTT : Connected to server");            
            client.loop();  
            delay(150);            
            Write_Log(client,"MQTT : Creating topics for HomeAssistant");

            //Slider Temperature
            
            //String number_topic = "homeassistant/number/Micronova/Controller/config";
            //String number_payload = "{\"name\": \"Temperature\", \"command_topic\": \"" cmd_topic "\", \"state_topic\": \"" settemp_topic "\", \"min\": 15, \"max\": 25, \"step\": 0.5, \"unit_of_measurement\": \"°C\", \"retain\": false, \"optimistic\": false, \"qos\": 0, \"icon\": \"mdi:thermometer\",  \"payload_template\": \"T{{ (value * 2) | int }}\", \"unique_id\": \"d5668e9c-153c-4330-ff53-a5bdfffa4413\", \"device\": " device_information "}";

            String select_topic = "homeassistant/select/micronova/Controller/config";
            String select_payload = "{\"name\": \"Temperature\", \"command_topic\": \"" cmd_topic "\", \"state_topic\": \"" settemp_topic "\",\"options\":[\"17\",\"18\",\"19\",\"20\",\"21\"],\"retain\": false, \"optimistic\": false, \"qos\": 0, \"icon\": \"mdi:thermometer\",  \"command_template\": \"T{{ (value | float) * 2 |int }}\", \"unique_id\": \"d5668e9c-153c-4330-ff53-a5bdfffa4413\", \"device\": " device_information "}";            
            client.publish(select_topic.c_str(), select_payload.c_str(), true);
            client.loop();  
            delay(150);            
        
            //Switch OnOff
            String switch_topic = "homeassistant/switch/Micronova/Controller/config";
            String switch_payload = "{\"name\": \"Controller\", \"command_topic\": \"" cmd_topic "\", \"state_topic\": \"" onoff_topic "\", \"payload_on\": \"ON\", \"payload_off\": \"OFF\", \"state_on\": \"ON\", \"state_off\": \"OFF\", \"retain\":false, \"optimistic\": false, \"qos\": 0, \"icon\": \"mdi:fire\", \"unique_id\": \"d5668e9c-843c-4330-ae53-a5bd135a4412\", \"device\": " device_information "}";
            client.publish(switch_topic.c_str(), switch_payload.c_str(), true);            
            client.loop();  
            delay(150);            
            
        //Sensor Etat
        String sensor_topic = "homeassistant/sensor/Micronova/Controller/config";
        String sensor_payload = "{\"name\": \"Controller\", \"state_topic\": \"" pong_topic "\", \"qos\": 0, \"icon\": \"mdi:power\", \"unique_id\": \"0038ec49-3921-4f1b-9fa9-71acb04052fa\",\"device\": " device_information "}";
        client.publish(sensor_topic.c_str(), sensor_payload.c_str(), true);
        client.loop();  
        delay(150);    
        
        //Sensor Temperature
        String temperature_sensor_topic = "homeassistant/sensor/Micronova/Temperature/config";
        String temperature_sensor_payload = "{\"name\": \"Temperature\", \"state_topic\": \"" ambtemp_topic "\", \"qos\": 0, \"device_class\": \"temperature\", \"state_class\": \"measurement\", \"unit_of_measurement\": \"°C\", \"icon\": \"mdi:thermometer\", \"unique_id\": \"9db3245e-6ace-4d14-ac07-844cd68d245d\",\"device\": " device_information "}";
        client.publish(temperature_sensor_topic.c_str(), temperature_sensor_payload.c_str(), true);
        client.loop();  
        delay(150);    
        
        //Sensor Consigne
        String consigne_sensor_topic = "homeassistant/sensor/Micronova/Consigne/config";
        String consigne_sensor_payload = "{\"name\": \"Consigne\", \"state_topic\": \"" tempset_topic "\", \"qos\": 0, \"device_class\": \"temperature\", \"state_class\": \"measurement\", \"unit_of_measurement\": \"°C\", \"icon\": \"mdi:thermometer\", \"unique_id\": \"9db3245e-6ace-4d14-ac07-844cd68d245e\",\"device\": " device_information "}";
        client.publish(consigne_sensor_topic.c_str(), consigne_sensor_payload.c_str(), true);    
        client.loop();  
        delay(150);            
        
        //Sensor Etat
        String state_sensor_topic = "homeassistant/sensor/Micronova/State/config";
        String state_sensor_payload = "{\"name\": \"State\", \"state_topic\": \"" state_topic "\", \"qos\": 0, \"icon\": \"mdi:fire-alert\", \"unique_id\": \"62fe5080-7668-409b-8451-323364b42eff\",\"device\": " device_information "}";
        client.publish(state_sensor_topic.c_str(), state_sensor_payload.c_str(), true);
        client.loop();  
        delay(150);    

        //Sensor Distance
        String dista_sensor_topic = "homeassistant/sensor/Micronova/Distance/config";
        String dista_sensor_payload = "{\"name\": \"Distance\", \"state_topic\": \"" distance_topic "\", \"qos\": 0, \"device_class\": \"distance\", \"state_class\": \"measurement\", \"unit_of_measurement\": \"mm\", \"icon\": \"mdi:arrow-expand-vertical\", \"unique_id\": \"9db3245e-6ace-4d14-ac07-844cd68d2442\",\"device\": " device_information "}";
        client.publish(dista_sensor_topic.c_str(), dista_sensor_payload.c_str(), true);
        client.loop();  
        delay(150);    

        //Sensor Qte
        String qte_sensor_topic = "homeassistant/sensor/Micronova/qte/config";
        String qte_sensor_payload = "{\"name\": \"Quantite\", \"state_topic\": \"" qte_topic "\", \"qos\": 0, \"unit_of_measurement\": \"%\", \"icon\": \"mdi:gauge\", \"unique_id\": \"9db3245e-6ace-4d14-ac07-424cd68d2442\",\"device\": " device_information "}";      
        client.publish(qte_sensor_topic.c_str(), qte_sensor_payload.c_str(), true);
        client.loop();  
        delay(150);    

        //Sensor log
        String log_sensor_topic = "homeassistant/sensor/Micronova/Log/config";
        String log_sensor_payload = "{\"name\": \"log\", \"state_topic\": \"" log_topic "\", \"qos\": 0, \"icon\": \"mdi:console\", \"unique_id\": \"9db3fffe-6ace-4d14-ac07-424cd68d2442\",\"device\": " device_information "}";      
        client.publish(log_sensor_topic.c_str(), log_sensor_payload.c_str(), true);
        client.loop();  
        delay(150);

        //Sensor Hauteur
        String hauteur_sensor_topic = "homeassistant/sensor/Micronova/hauteur/config";
        String hauteur_sensor_payload = "{\"name\": \"log\", \"state_topic\": \"" hauteur_topic "\", \"qos\": 0, \"icon\": \"mdi:console\", \"unique_id\": \"42b3fffe-6ace-4314-a427-424cd68d2442\",\"retain\":true,\"device\": " device_information "}";      
        client.publish(hauteur_sensor_topic.c_str(), hauteur_sensor_payload.c_str(), true);
        client.loop();  
        delay(150);       

        Write_Log(client,"MQTT : HomeAssistant topics created.");
        }
        Write_Log(client,"MQTT : Configuration terminated");

        client.subscribe(cmd_topic);
        Write_Log(client,"MQTT : CMD Topic subscribed");
        
        client.subscribe(hauteur_topic);
        Write_Log(client,"MQTT : hauteur Topic subscribed");
        
    }

//Callback Function
    void callback(char *topic, byte *payload, unsigned int length)
    {
    if ((char)payload[0] == 'P') // Power payload Prévoir lettre P devant la valeur pour valider le choix puissance
    {
        Write_Log(client,"CallBack : P message");
        if (length == 2)
        {
        const char *stovePW = NULL;
        if ((char)payload[1] == '0')
            {
                Write_Log(client,"CallBack : P0");
                stovePW = stovePWA;
            }
        else if ((char)payload[1] == '1')
            {
                Write_Log(client,"CallBack : P1");
                stovePW = stovePW1;
            }
        else if ((char)payload[1] == '2')
            {
                Write_Log(client,"CallBack : P2");
                stovePW = stovePW2;
            }
        else if ((char)payload[1] == '3')
            {
                Write_Log(client,"CallBack : P3");
                stovePW = stovePW3;
            }
        else if ((char)payload[1] == '4')
            {
            Write_Log(client,"CallBack : P4");
            stovePW = stovePW4;
            }
        else if ((char)payload[1] == '5')
            {
            Write_Log(client,"CallBack : P5");
            stovePW = stovePW5;
            }

        for (int i = 0; i < 4; i++)
            {
            StoveSerial.write(stovePW[i]);
            delay(1);
            }
            delay(2000);
            //getStates();
        }
    }
    else if ((char)payload[0] == 'T') // Temperature target payload prevoir T devant la valeur pour valider le choix température
    {   
        /*
        if (length == 3)
        {
            
            Serial.print("CallBack : T message, payload = ");
            for (unsigned int i = 0; i < length; i++) Serial.print((char)payload[i]);
            Serial.println();

            // Convertir directement la chaîne après 'T' en entier
            uint8_t tempValue = atoi((char*)payload + 1); // T37 → 37

            // Préparer la trame pour le poêle
            const char temperatureSet[4] = {_EEPROMW, tempSetAddr, tempValue, (char)(_EEPROMW + tempSetAddr + tempValue)};

            // Envoi
            StoveSerial.write(temperatureSet, 4);
            delay(10);

            Serial.print("Trame envoyée au poêle : ");
            for (int i = 0; i < 4; i++) Serial.print((int)temperatureSet[i], HEX), Serial.print(" ");
            Serial.println();

            // Rafraîchir les états
            getStates();

        }
        else
        {
            Write_Log(client,"CallBack : T FLAG NOT 3");        
        }

        */

            Serial.print("CallBack : T message, payload = ");
        for (unsigned int i = 0; i < length; i++)
            {
            Serial.print((char)payload[i]);
            }
        Serial.println();

        // Conversion robuste : "36", "36.0", "36.5" → float
        float rawValue = atof((char*)payload + 1);

        // Arrondi et conversion Micronova
        uint8_t tempValue = (uint8_t)round(rawValue);

        Serial.print("Température décodée : ");
        Serial.print(rawValue);
        Serial.print(" → Micronova = ");
        Serial.println(tempValue);

        // Préparer la trame pour le poêle
        const char temperatureSet[4] = {
            _EEPROMW,
            tempSetAddr,
            tempValue,
            (char)(_EEPROMW + tempSetAddr + tempValue)
        };

        // Envoi vers le poêle
        StoveSerial.write(temperatureSet, 4);
        delay(10);

        Serial.print("Trame envoyée au poêle : ");
        for (int i = 0; i < 4; i++)
        {
            Serial.print((int)temperatureSet[i], HEX);
            Serial.print(" ");
        }
        Serial.println();

        // Rafraîchir les états
        getStates();
        
    }
    else if ((char)payload[0] == 'O' && (char)payload[1] == 'N') // Switch ON payload  Valeur de l'action ON
    {
        Write_Log(client,"CallBack : ON message");
        for (int i = 0; i < 4; i++)
        {
                if (stoveState > 10)  //si hors plage fonctionnement, passe sur marche
                {
                    StoveSerial.write(stoveOn[i]);
                    delay(1);
                }
                else if (stoveState == 0)  //si hors plage fonctionnement, passe sur marche
                {
                    StoveSerial.write(stoveOn[i]);
                    delay(1);
                }
        }
                client.publish(onoff_topic, "Marche", true);
                delay(2000);
                getStates();
    }
    else if ((char)payload[0] == 'S' && (char)payload[1] == 'T') // Stand by payload  Valeur de l'action STB
    {
        Write_Log(client,"CallBack : STB message");
        for (int i = 0; i < 4; i++)
        {
                if (stoveState < 10)   // si dans plage de fonctionnement, passe sur stb
                {
                if (stoveState > 0)  // si en marche, passe sur stb
                {
                    StoveSerial.write(stoveStb[i]);
                    delay(1);
                }
                }
        }
            client.publish(onoff_topic, "Attente", true);
            delay(2000);
            getStates();
    }
    else if ((char)payload[0] == 'O' && (char)payload[1] == 'F') // Switch OFF payload  Valeur de l'action OFF
    {
        Write_Log(client,"CallBack : OFF message");
        for (int i = 0; i < 4; i++)
            {
                    StoveSerial.write(forceOff[i]);  // On passe directement sur eteint
                    delay(1);
            }  
        client.publish(onoff_topic, "Eteint", true);  //Publies eteint
        delay(2000);
        getStates();     
    }
    else if ((char)payload[0] == 'H') // Temperature target payload prevoir T devant la valeur pour valider le choix température
    {   
        if (length == 3)
        {
            ///////////////////////////////////////////////////////////////////////////////////
            Serial.print("CallBack : H message, payload = ");
            for (unsigned int i = 0; i < length; i++) Serial.print((char)payload[i]);
            Serial.println();

            // Convertir directement la chaîne après 'H' en entier
            uint8_t tempValue = atoi((char*)payload + 1); // T37 → 37
            // Rafraîchir les états
            getStates();
        }
        else
        {
            Write_Log(client,"CallBack : T FLAG NOT 3");        
        }
    }


} 
   


void checkStoveReply() 
{
    uint8_t rxCount = 0;
    stoveRxData[0] = 0x00;
    stoveRxData[1] = 0x00;
    //while (StoveSerial.available()) 
    while (StoveSerial.available() && rxCount < 2)
    {
        stoveRxData[rxCount] = StoveSerial.read();
        rxCount++;
    }
    digitalWrite(ENABLE_RX, HIGH);
    if (rxCount == 2)
    {
        byte val = stoveRxData[1];
        byte checksum = stoveRxData[0];
        byte param = checksum - val;
     
        switch (param)
        {
        case ambTempAddr:
            ambTemp = val;
            Write_Log(client,"checkStoveReply : Temperature Ambiance : " + String(ambTemp));
			temperature=String(ambTemp/2.0);
            client.publish(ambtemp_topic, String(ambTemp/2.0).c_str(), true);
            break;

        case fumeTempAddr:
            fumeTemp = val;
            Write_Log(client,"checkStoveReply : Temperature Fumee : " + String(fumeTemp));
            client.publish(fumetemp_topic, String(fumeTemp).c_str(), true);
            break; 
			
        case blowTempAddr:
            blowTemp = val;
            Write_Log(client,"checkStoveReply : Temperature Soufflerie : " + String(blowTemp));
            client.publish(blowtemp_topic, String(blowTemp).c_str(), true);
            break; 

        case cardTempAddr:
            cardTemp = val;
            Write_Log(client,"checkStoveReply : Temperature ESP Poele : " + String(cardTemp));
            client.publish(cardtemp_topic, String(cardTemp/2.0).c_str(), true);
            break; 

	      case extractorAddr:
            extractor = val;
            client.publish(extractor_topic, String(extractor).c_str(), true);
            break; 

        case blowSpeedAddr:
            blowSpeed = val;
            client.publish(blowspeed_topic, String(blowSpeed).c_str(), true);
            break; 

        case visPelletAddr:
            visPellet = val;
            client.publish(vispellet_topic, String(visPellet).c_str(), true);
            break; 
			   
        case fixVacuumAddr:
            fixVac = val;
            client.publish(fixvac_topic, String(fixVac).c_str(), true);
            break; 

        case readVacuumAddr:
            readVac = val;
            client.publish(readvac_topic, String(readVac).c_str(), true);
            break; 
			
	    case multVacuumAddr:
            multVac = val;
            client.publish(multvac_topic, String(multVac).c_str(), true);
            break; 
	    
	    case tempSetAddr + _EEPROMR:
            tempSet = val;
            Write_Log(client,"checkStoveReply : Consigne Temperature : " + String(tempSet));
			consigne=String(tempSet/2.0);
            client.publish(tempset_topic, String(tempSet / 2.0).c_str(), true);
            break;
			
    	case pwrSetAddr + _EEPROMR:
            pwrSet = val; 
            Write_Log(client,"checkStoveReply : Consigne Puissance : " + String(pwrSet));
            client.publish(pwrset_topic, String(pwrSet).c_str(), true);
            break;	
		
    	case stoveStateAddr:
            stoveState = val;
            switch (stoveState)
            {
            case 0:
                // client.publish(state_topic, "Eteint", true);
                // Write_Log(client,"checkStoveReply : Eteint");
                //delay(1000);                
				stove_state="Eteint";
                client.publish(onoff_topic, "Eteint", true);
			          break;
            case 1:
                // client.publish(state_topic, "Ventillation Initiale", true);
                // Write_Log(client,"checkStoveReply : Ventilation initiale");
                //delay(1000);
				stove_state="Ventilation initiale.";
                client.publish(onoff_topic, "Marche", true);
                Write_Log(client,"checkStoveReply : Etat : Marche");
				        break;
            case 2:
                // client.publish(state_topic, "Prechauf. Resist.", true);
                // Write_Log(client,"checkStoveReply : Préchauffage résistance");
                //delay(1000);
				stove_state="Prechauffage résistance.";
                client.publish(onoff_topic, "Marche", true);
                Write_Log(client,"checkStoveReply : Etat : Marche");
				        break;
            case 3:
                // client.publish(state_topic, "Precharg. Pellet", true);
                // Write_Log(client,"checkStoveReply : Chargement Pellet");
                //delay(1000);
				stove_state="Chargement Pellet";
                client.publish(onoff_topic, "Marche", true);
                Write_Log(client,"checkStoveReply : Marche");
			        	break;
            case 4:
                // client.publish(state_topic, "Attente Flamme", true);
                // Write_Log(client,"checkStoveReply : Attente flamme.");
                //delay(1000);
				stove_state="Attente flamme.";
                client.publish(onoff_topic, "Marche", true);
                Write_Log(client,"checkStoveReply : Marche");
				        break;
            case 5:
                // client.publish(state_topic, "Attente Allumage", true);
                // Write_Log(client,"checkStoveReply : Attente Allumage");
                //delay(1000);
				stove_state="Attente allumage";
                client.publish(onoff_topic, "Marche", true);
                Write_Log(client,"checkStoveReply : Marche");
			         break;
            case 6:
                // client.publish(state_topic, "Allumage", true);
                // Write_Log(client,"checkStoveReply : Alumage");
                //delay(1000);
				stove_state="Allumage";
                client.publish(onoff_topic, "Marche", true);
                Write_Log(client,"checkStoveReply : Marche");
				        break;
            case 7:
                // client.publish(state_topic, "Chauffage", true);
                // Write_Log(client,"checkStoveReply : Chauffage");
                //delay(1000);
				stove_state="Chauffage";
                client.publish(onoff_topic, "Marche", true);
                Write_Log(client,"checkStoveReply : Marche");
				        break;
            case 8:
                // client.publish(state_topic, "Nettoyage Creuset", true);
                // Write_Log(client,"checkStoveReply : Nettoyage Creuset");
                //delay(1000);
				stove_state="Nettoyage Creuset";
                client.publish(onoff_topic, "Marche", true);
                Write_Log(client,"checkStoveReply : Marche");
			        	break;
            case 9:
                // client.publish(state_topic, "Nettoyage Final", true);
                // Write_Log(client,"checkStoveReply : Nettoyage Final");
                //delay(1000);
				stove_state="Nettoyage final";
                client.publish(onoff_topic, "Marche", true);
                Write_Log(client,"checkStoveReply : Marche");
				        break;
            case 10:
                // client.publish(state_topic, "Attente Stand-By", true);
                // Write_Log(client,"checkStoveReply : Attente Stand By");
                //delay(1000);
				stove_state="Attente stand-by";
                client.publish(onoff_topic, "Marche", true);
                Write_Log(client,"checkStoveReply : Marche");
				        break;
            case 11:
                // client.publish(state_topic, "Stand-By", true);
                // Write_Log(client,"checkStoveReply : Stand By");
                //delay(1000);
				stove_state="Stand By";
                client.publish(onoff_topic, "Arrêt", true);
                Write_Log(client,"checkStoveReply : Arrêt");
                break;
            case 12:
                // client.publish(state_topic, "Manque Pellet", true);
                // Write_Log(client,"checkStoveReply : Manque Pellet");
                //delay(1000);
				stove_state="Manque Pellets";
                client.publish(onoff_topic, "Arrêt", true);
                Write_Log(client,"checkStoveReply : Arrêt");
                break;
            case 13:
                // client.publish(state_topic, "Manque Allumage", true);
                // Write_Log(client,"checkStoveReply : Manque Allumage");
                //delay(1000);
				stove_state="Manque Allumage";
                client.publish(onoff_topic, "Marche", true);
                Write_Log(client,"checkStoveReply : Marche");
                break;
           	}
				client.publish(state_topic, String(stove_state).c_str(), true);
				Write_Log(client,"checkStoveReply : "+ stove_state);
				break;
				
	    case stovePwrAddr:
            stovePower = val;
            switch (stovePower)
            {
            case 1:
                client.publish(stovepower_topic, "Eco", true);
                //delay(1000);
                break;
            case 2:
                client.publish(stovepower_topic, "P1", true);
                //delay(1000);
                break;
            case 3:
                client.publish(stovepower_topic, "P2", true);
                //delay(1000);
                break;
            case 4:
                client.publish(stovepower_topic, "P3", true);
                //delay(1000);
                break;
            case 5:
                client.publish(stovepower_topic, "P4", true);
                //delay(1000);
                break;
            case 6:
                client.publish(stovepower_topic, "P5", true);
                //delay(1000);
                break;
            }
				break;
        }
     } 
}

void getStoveState() //Get detailed stove state
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(stoveStateAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getAmbTemp() //Get room temperature
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(ambTempAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getStovePower() //Get Puissance réelle
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(stovePwrAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getFumeTemp() //Get fume temperature
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(fumeTempAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getBlowTemp() //Get blow temperature
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(blowTempAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getCardTemp() //Get card temperature
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(cardTempAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getExtractor() //Get extractor speed
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(extractorAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getBlowSpeed() //Get blow speed
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(blowSpeedAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getVisPellet() //Get speed vis pellet
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(visPelletAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getFixVacuum() //Get speed vis pellet
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(fixVacuumAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getReadVacuum() //Get speed vis pellet
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(readVacuumAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getMultVacuum() //Get speed vis pellet
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(multVacuumAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getTempSet() //Get the thermostat setting
{
    const byte readByte = _EEPROMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(tempSetAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getPowerSet() //Get the Power setting
{
    const byte readByte = _EEPROMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(pwrSetAddr);
    digitalWrite(ENABLE_RX, LOW);
    delay(80);
    checkStoveReply();
}

void getQte() //Get speed vis pellet
{
    if (measure_sleep==0)
    {
        measure_sleep++;
        VL53L0X_RangingMeasurementData_t measure;        
        lox.rangingTest(&measure, false);
        
        distance=measure.RangeMilliMeter;
        if (distance>1000)
        {
            count_reboot++;
            Write_Log(client,"getQte: Measure Error. NB reboots="+ String(count_reboot));        
        }
        else
        {            
            if (measure.RangeStatus != 4)
            {
                count_reboot=0;
                
                Write_Log(client,"getQte: Distance "+ String(distance) + " mm.");
                client.publish(distance_topic, String(distance).c_str(), true);
                    
                float qte = ((float)(hauteur_cuve - distance) / (float)hauteur_cuve) * 100;
                /*
                Write_Log(client,"getQte: Qte "+ String(qte) + " %.");
                client.publish(qte_topic, String(qte).c_str(), true);
                */
                if (lastQte < 0 || abs(qte - lastQte) >= 10.0)
                {
                    Write_Log(client,"getQte: Qte " + String(qte) + " %.");
                    client.publish(qte_topic, String(qte).c_str(), true);

                    lastQte = qte; // Mise à jour de la dernière valeur
                }
                else
                {
                    Write_Log(client,"getQte: Qte variation < 10%, pas de publication.");
                }
                
            } else
            {
                Write_Log(client,"getQte: Out of range");        
            }
        }
        
    }
    else
    {
        measure_sleep++;
        if (measure_sleep > 10) measure_sleep = 0; // Réinitialise après 10 cycles pour refaire une mesure
        Write_Log(client,"getQte: MeasureSleep. Sleep="+String(measure_sleep));          
    }
    delay(200);
}

 

void getStates() //Calls all the get…() functions
{
    getStovePower();
    getStoveState();
    getAmbTemp();
    getTempSet();
    getPowerSet();
    getQte();  // Mesure du VL53L0X
}

//Page Web (HTML minimaliste)
String htmlPage()
{
    String page = "<!DOCTYPE html><html><head>";
    page += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
    page += "<title>Micronova - Configuration</title>";

    // Ajout de styles CSS
    page += "<style>";
    page += "body { font-family: Arial, sans-serif; background-color: #f4f4f9; color: #333; text-align: center; padding: 20px; }";
    page += "h2 { color: #2c3e50; }";
    page += "form { background: white; padding: 20px; margin: 20px auto; border-radius: 10px; box-shadow: 0 2px 8px rgba(0,0,0,0.2); max-width: 400px; }";
    page += "input[type=number], input[type=text] { width: 100%; padding: 10px; margin: 10px 0; border-radius: 5px; border: 1px solid #ccc; }";
    page += "input[type=submit] { background-color: #3498db; color: white; padding: 10px 20px; border: none; border-radius: 5px; font-size: 16px; cursor: pointer; transition: background 0.3s; }";
    page += "input[type=submit]:hover { background-color: #2980b9; }";
    page += ".reboot-btn { background-color: #e74c3c; }";
    page += ".reboot-btn:hover { background-color: #c0392b; }";
    page += ".card { background: #fff; padding: 15px; margin: 15px auto; border-radius: 10px; box-shadow: 0 2px 6px rgba(0,0,0,0.2); max-width: 400px; }";
    page += "p { font-size: 18px; margin: 10px 0; }";
    page += "b { color: #2c3e50; }";
    page += "</style>";

    page += "</head><body>";
    page += "<h2>ESP Poele</h2>";

    // Carte pour les dernieres mesures
    page += "<div class='card'>";
	page += "<p>Hauteur cuve : <b>" + String(hauteur_cuve) + " mm</b></p>";
    page += "<p>Hauteur lue : <b>" + String(distance) + " mm</b></p>";
	page += "<p>Quantite restante : <b>" + String(lastQte, 1) + " %</b></p>";
	page += "<p>Consigne : <b>" + String(consigne) + " C</b></p>";
	page += "<p>Temperature : <b>" + String(temperature) + " C</b></p>";
	page += "<p>Etat du poele : <b>" + String(stove_state) + "</b></p>";
    page += "<p>Hostname : <b>" + espHostname + "</b></p>";	
    page += "</div>";
	
	// Formulaire pour la hauteur
    page += "<form action='/set' method='POST'>";
    page += "<label>Hauteur cuve (mm):</label><br>";
    page += "<input type='number' name='hauteur' value='" + String(hauteur_cuve) + "' min='100' max='2000'><br><br>";
    page += "<input type='submit' value='Enregistrer'>";
    page += "</form>";

    // Formulaire pour le hostname
    page += "<form action='/hostname' method='POST'>";
    page += "<label>Hostname (lettres, chiffres, tirets) :</label><br>";
    page += "<input type='text' name='hostname' value='" + espHostname + "' maxlength='32' required><br><br>";
    page += "<input type='submit' value='Enregistrer'>";
    page += "</form>";

    // Formulaire pour le reboot
    page += "<form action='/reboot' method='POST'>";
    page += "<input type='submit' value='Redemarrer' class='reboot-btn'>";
    page += "</form>";
    
    page += "<p><a href='/update'>Mise a jour du firmware (OTA)</a></p>";

    page += "</body></html>";

    return page;
}

//Handlers HTTP
void handleRoot()
{
    Serial.println("HTTP / appelé");
    webServer.send(200, "text/html", htmlPage());
}

//Réception du formulaire /set
void handleSet()
{
    Serial.println("HTTP hauteur appelé");
    if (webServer.hasArg("hauteur"))
    {
        int newHauteur = webServer.arg("hauteur").toInt();

        if (newHauteur > 100 && newHauteur < 2000) // sécurité
        {
            hauteur_cuve = newHauteur;
            hauteur_set = true;

            Write_Log(client, "WEB : hauteur_cuve mise à jour = " + String(hauteur_cuve));
        }
    }
    webServer.sendHeader("Location", "/");
    webServer.send(303);
}

//Reception du formulaire /hostname
void handleHostname()
{
    Serial.println("HTTP hostname appele");
    if (webServer.hasArg("hostname"))
    {
        String newHostname = webServer.arg("hostname");
        newHostname.trim();

        if (!isValidHostname(newHostname))
        {
            webServer.send(400, "text/html",
                "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1'></head><body>"
                "<h2>Hostname invalide</h2>"
                "<p>1 a 32 caracteres : lettres, chiffres et tirets (pas de tiret au debut ou a la fin).</p>"
                "<p><a href='/'>Retour</a></p>"
                "</body></html>"
            );
            return;
        }

        if (newHostname != espHostname)
        {
            saveHostname(newHostname);
            Write_Log(client, "WEB : hostname modifie = " + newHostname + " (redemarrage)");

            // Le nouveau hostname est applique au redemarrage (annonce DHCP)
            webServer.send(200, "text/html",
                "<!DOCTYPE html><html><head>"
                "<meta http-equiv='refresh' content='8;url=/' />"
                "</head><body>"
                "<h2>Hostname enregistre</h2>"
                "<p>L'ESP redemarre pour appliquer le nouveau nom.</p>"
                "<p>Redirection automatique dans 8 secondes.</p>"
                "</body></html>"
            );
            delay(1000);   // laisse le temps d'envoyer la page
            ESP.restart();
        }
    }
    webServer.sendHeader("Location", "/");
    webServer.send(303);
}

void handleReboot()
{
    Serial.println("HTTP reboot appelé");
     webServer.send(200, "text/html",
        "<!DOCTYPE html><html><head>"
        "<meta http-equiv='refresh' content='5;url=/' />"
        "<script>"
        "setTimeout(function(){ window.location.href = '/'; }, 5000);"
        "</script>"
        "</head><body>"
        "<h2>Redemarrage en cours...</h2>"
        "<p>L'ESP va redemarrer.</p>"
        "<p>Redirection automatique dans 5 secondes.</p>"
        "</body></html>"
    );
    
    Write_Log(client, "WEB : Redémarrage ESP demandé");
    delay(1000);   // laisse le temps d'envoyer la page
    ESP.restart();
}

void setup()
{
    //delay(10e3);
    pinMode(ENABLE_RX, OUTPUT);
    //Init Serial Communication
    digitalWrite(ENABLE_RX, HIGH); //The led of the optocoupler is off
    Serial.begin(115200);
    StoveSerial.begin(1200, SERIAL_MODE, RX_PIN, TX_PIN, false, 256);

    //Init Wifi
    loadHostname();
    setup_wifi();

    webServer.on("/", handleRoot);
    webServer.on("/set", HTTP_POST, handleSet);
    webServer.on("/hostname", HTTP_POST, handleHostname);
    webServer.on("/reboot", HTTP_POST, handleReboot);
    httpUpdater.setup(&webServer, "/update", ota_user, ota_pass);
    webServer.begin();

    // OTA depuis l'IDE Arduino / PlatformIO
    ArduinoOTA.setHostname(espHostname.c_str());
    ArduinoOTA.setPassword(ota_pass);
    ArduinoOTA.onStart([]() { Write_Log(client, "OTA : debut de la mise a jour"); });
    ArduinoOTA.onEnd([]() { Write_Log(client, "OTA : termine, redemarrage"); });
    ArduinoOTA.onError([](ota_error_t error) { Write_Log(client, "OTA : erreur " + String((int)error)); });
    ArduinoOTA.begin();

    Write_Log(client, "WEB : Serveur HTTP démarré");

                                                                                                    //Init MQTT
                                                                                                    /*
                                                                                                    client.setServer(mqtt_server, mqtt_port);
                                                                                                    client.setCallback(callback);
                                                                                                    client.connect("ESPClient", mqtt_user,mqtt_pass);
                                                                                                    client.subscribe(cmd_topic);
                                                                                                    */
    
    
    reconnect_mqtt();
    client.subscribe(cmd_topic);
    client.subscribe(hauteur_topic); // abonner au topic hauteur
    

    // Petit délai pour recevoir le dernier message MQTT sur hauteur
    unsigned long start = millis();
    while (millis() - start < 2000) { // attendre 2 secondes max
        client.loop(); // process MQTT messages
    }

    //Write logs
    
    Write_Log(client,"Setup : Wifi ok");        
    Write_Log(client,"Setup : MQTT ok");        
    

    //Init VL53L0X sensor  
    
    Wire.begin(SDA, SCL);   // SDA = D6, SCL = D1
    Wire.setClock(100000);    
    if (!lox.begin()) {
        Write_Log(client,"Setup VL53L0X : KO");                
    }
    else
    {
        Write_Log(client,"Setup VL53L0X : ok");       
    }   

    count_reboot=0;
    Write_Log(client,"Setup count_reboot=0"); 

    measure_sleep=0;
    Write_Log(client,"Setup MeasureSleep=0"); 
      
}


void loop()
{
    
    
    if (!client.connected())
    {
        reconnect_mqtt();
		client.subscribe(cmd_topic);
    }
    
        client.loop();
        webServer.handleClient();
        ArduinoOTA.handle();
   
    
    unsigned long currentMillis = millis();
    if (previousMillis > currentMillis)
    {
        previousMillis = 0;
    }
    if (currentMillis - previousMillis >= 60000)  // délais entre chaque cycle 60s
    {
        previousMillis = currentMillis;
        getStates();
        client.publish(pong_topic, "Connecté");
        Write_Log(client,"Loop : Alive");        
        if (count_reboot >10)
        {
            Write_Log(client,"Loop : Too many failures, stopping.");        
            delay(1000);
            ESP.restart();  
        }
    }
    
}