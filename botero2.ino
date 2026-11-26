//Include libraries
    #include <ESP8266WiFi.h>
    #include <DNSServer.h>             // portail captif (mode configuration)
    #include <PubSubClient.h>
    #include <SoftwareSerial.h>
    #include <Wire.h>    
    #include <Adafruit_VL53L0X.h>
    #include <ESP8266WebServer.h>
    #include <EEPROM.h>
    #include <ArduinoOTA.h>
    #include <ESP8266HTTPUpdateServer.h>

//Plus aucun identifiant dans le code : Wi-Fi, MQTT, broches et mots de passe sont saisis sur la page de
//configuration (point d'acces "Micronova-Setup" au premier demarrage) et conserves en EEPROM.

//Version du firmware (a incrementer a chaque nouvelle version)
    #define FW_VERSION "0.14"


//MQTT Variables
    #define mqtt_topic "micronova"
    #define DEFAULT_MQTT_PORT 1883
    #define MQTT_CLIENT_ID "ESPClient"

//Brochage de l'ESP : numeros de GPIO, saisis sur la page de configuration et conserves en EEPROM.
//Les valeurs ci-dessous sont celles du montage d'origine : elles servent de valeurs par defaut.
    SoftwareSerial StoveSerial;
    #define SERIAL_MODE SWSERIAL_8N2 //8 data bits, parity none, 2 stop bits
    #define DEFAULT_PIN_RX D3          // liaison poele, reception
    #define DEFAULT_PIN_TX D4          // liaison poele, emission
    #define DEFAULT_PIN_ENABLE_RX D2   // activation de la reception (optocoupleur)
    #define DEFAULT_PIN_SDA D6         // I2C : capteur VL53L0X
    #define DEFAULT_PIN_SCL D1
    uint8_t pinRx = DEFAULT_PIN_RX;
    uint8_t pinTx = DEFAULT_PIN_TX;
    uint8_t pinEnableRx = DEFAULT_PIN_ENABLE_RX;
    uint8_t pinSda = DEFAULT_PIN_SDA;
    uint8_t pinScl = DEFAULT_PIN_SCL;

//Point d'acces de configuration (premier demarrage, ou Wi-Fi injoignable)
    #define AP_SSID "Micronova-Setup"
    #define AP_PASS ""                            // vide = point d'acces ouvert ; sinon 8 caracteres minimum
    #define WIFI_CONNECT_TIMEOUT_MS 30000UL       // delai de connexion au Wi-Fi saisi avant de rouvrir le point d'acces
    #define PORTAL_RETRY_MS 300000UL              // point d'acces de repli : nouvel essai de connexion apres 5 min d'inactivite


//Capteur de distance
Adafruit_VL53L0X lox;
int  hauteur_cuve=500;
ESP8266WebServer webServer(80);
ESP8266HTTPUpdateServer httpUpdater;   // mise a jour OTA via la page web (/update)
bool hauteur_set = false;        // indique si modifiée via web

//Hostname (valeur par defaut + stockage en EEPROM)
    #define DEFAULT_HOSTNAME "micronova"
    #define HOSTNAME_MAX_LEN 32
    #define EEPROM_MAGIC 0xA5   // marqueur : un hostname valide est stocke en EEPROM
    String espHostname = DEFAULT_HOSTNAME;

//Hauteur de la cuve en EEPROM (octet 40 = marqueur, octets 41-42 = valeur en mm, poids faible puis fort)
//Le hostname occupe les octets 0 a 33 : pas de chevauchement.
    #define HAUTEUR_ADDR 40
    #define HAUTEUR_MAGIC 0xB7

//Configuration saisie sur la page web (Wi-Fi, MQTT, broches, identifiants web/OTA), stockee en EEPROM
//apres la zone du hostname (octets 0 a 33) et de la hauteur de cuve (octets 40 a 42) : les valeurs deja
//enregistrees par les versions precedentes sont conservees.
    #define CFG_ADDR 64
    #define CFG_MAGIC 0xC0F10012UL      // marqueur : une configuration est stockee en EEPROM
    struct StoredCfg
    {
        uint32_t magic;
        char ssid[33];
        char pass[64];
        char mqttHost[65];
        uint16_t mqttPort;
        char mqttUser[33];
        char mqttPass[64];
        uint8_t pinRx, pinTx, pinEnable, pinSda, pinScl;
        char webUser[33];
        char webPass[64];
    };
    #define EEPROM_SIZE (CFG_ADDR + sizeof(StoredCfg))

    String cfgSsid, cfgPass;
    String cfgMqttHost, cfgMqttUser, cfgMqttPass;
    uint16_t cfgMqttPort = DEFAULT_MQTT_PORT;
    String cfgWebUser = "admin", cfgWebPass;     // identifiants (facultatifs) de la page web, de /update et de l'OTA ; mot de passe vide = acces libre
    bool cfgConfigured = false;                  // une configuration complete est stockee en EEPROM

//Mode configuration (point d'acces + page /config, sans MQTT ni dialogue avec le poele)
    bool portalMode = false;
    bool portalFirstSetup = false;               // true : aucune configuration stockee, donc aucun mot de passe a demander
    unsigned long lastWebActivity = 0;
    DNSServer dnsServer;
    String scanCache = "[]";                     // derniere liste de reseaux Wi-Fi (JSON)

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

//Lit la hauteur de cuve en EEPROM (garde la valeur par defaut si absente ou hors plage)
void loadHauteur()
{
    if (EEPROM.read(HAUTEUR_ADDR) == HAUTEUR_MAGIC)
    {
        int stored = EEPROM.read(HAUTEUR_ADDR + 1) | (EEPROM.read(HAUTEUR_ADDR + 2) << 8);
        if (stored > 100 && stored < 2000) hauteur_cuve = stored;
    }
}

//Enregistre la hauteur de cuve en EEPROM (n'ecrit que si la valeur a change, pour menager la flash)
void saveHauteur(int valeur)
{
    int stored = -1;
    if (EEPROM.read(HAUTEUR_ADDR) == HAUTEUR_MAGIC)
        stored = EEPROM.read(HAUTEUR_ADDR + 1) | (EEPROM.read(HAUTEUR_ADDR + 2) << 8);
    if (stored == valeur) return;

    EEPROM.write(HAUTEUR_ADDR, HAUTEUR_MAGIC);
    EEPROM.write(HAUTEUR_ADDR + 1, valeur & 0xFF);
    EEPROM.write(HAUTEUR_ADDR + 2, (valeur >> 8) & 0xFF);
    EEPROM.commit();
}

//Broches utilisables : numeros de GPIO de D1 a D8 sur une carte NodeMCU / Wemos D1 mini.
//GPIO16 (D0) est exclu : pas d'interruption, donc inutilisable pour la liaison serie logicielle ni pour l'I2C.
bool isValidPin(int gpio)
{
    switch (gpio)
    {
        case 0: case 2: case 4: case 5: case 12: case 13: case 14: case 15: return true;
        default: return false;
    }
}

//Les cinq fonctions doivent utiliser cinq broches differentes
bool pinsAreDistinct(int a, int b, int c, int d, int e)
{
    int p[5] = {a, b, c, d, e};
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++)
            if (p[i] == p[j]) return false;
    return true;
}

//Lit la configuration en EEPROM. cfgConfigured reste a false si aucune configuration complete n'est stockee.
//Les valeurs par defaut (broches d'origine, port 1883) restent en place pour tout champ absent ou invalide.
void loadConfig()
{
    StoredCfg sc;
    EEPROM.get(CFG_ADDR, sc);
    cfgConfigured = false;
    if (sc.magic != CFG_MAGIC) return;

    sc.ssid[sizeof(sc.ssid) - 1] = 0;
    sc.pass[sizeof(sc.pass) - 1] = 0;
    sc.mqttHost[sizeof(sc.mqttHost) - 1] = 0;
    sc.mqttUser[sizeof(sc.mqttUser) - 1] = 0;
    sc.mqttPass[sizeof(sc.mqttPass) - 1] = 0;
    sc.webUser[sizeof(sc.webUser) - 1] = 0;
    sc.webPass[sizeof(sc.webPass) - 1] = 0;

    cfgSsid = sc.ssid;
    cfgPass = sc.pass;
    cfgMqttHost = sc.mqttHost;
    cfgMqttUser = sc.mqttUser;
    cfgMqttPass = sc.mqttPass;
    cfgWebUser = sc.webUser;
    cfgWebPass = sc.webPass;
    if (cfgWebUser.length() == 0) cfgWebUser = "admin";   // valeur proposee dans le formulaire
    if (sc.mqttPort > 0) cfgMqttPort = sc.mqttPort;

    if (isValidPin(sc.pinRx) && isValidPin(sc.pinTx) && isValidPin(sc.pinEnable) && isValidPin(sc.pinSda) && isValidPin(sc.pinScl)
        && pinsAreDistinct(sc.pinRx, sc.pinTx, sc.pinEnable, sc.pinSda, sc.pinScl))
    {
        pinRx = sc.pinRx;
        pinTx = sc.pinTx;
        pinEnableRx = sc.pinEnable;
        pinSda = sc.pinSda;
        pinScl = sc.pinScl;
    }

    cfgConfigured = cfgSsid.length() > 0 && cfgMqttHost.length() > 0;   // le compte web est facultatif
}

//Enregistre la configuration en EEPROM (les variables cfg* ne sont pas modifiees : l'ESP redemarre ensuite)
void saveConfig(const String &ssid, const String &pass, const String &mHost, uint16_t mPort, const String &mUser, const String &mPass, uint8_t rx, uint8_t tx, uint8_t en, uint8_t sda, uint8_t scl, const String &wUser, const String &wPass)
{
    StoredCfg sc;
    memset(&sc, 0, sizeof(sc));
    sc.magic = CFG_MAGIC;
    strncpy(sc.ssid, ssid.c_str(), sizeof(sc.ssid) - 1);
    strncpy(sc.pass, pass.c_str(), sizeof(sc.pass) - 1);
    strncpy(sc.mqttHost, mHost.c_str(), sizeof(sc.mqttHost) - 1);
    sc.mqttPort = mPort;
    strncpy(sc.mqttUser, mUser.c_str(), sizeof(sc.mqttUser) - 1);
    strncpy(sc.mqttPass, mPass.c_str(), sizeof(sc.mqttPass) - 1);
    sc.pinRx = rx;
    sc.pinTx = tx;
    sc.pinEnable = en;
    sc.pinSda = sda;
    sc.pinScl = scl;
    strncpy(sc.webUser, wUser.c_str(), sizeof(sc.webUser) - 1);
    strncpy(sc.webPass, wPass.c_str(), sizeof(sc.webPass) - 1);

    EEPROM.put(CFG_ADDR, sc);
    EEPROM.commit();
}

//Efface la configuration : au prochain demarrage, l'ESP rouvre le portail de configuration initiale.
//Le hostname et la hauteur de cuve ne sont pas touches (ils pre-remplissent le formulaire).
void clearConfig()
{
    StoredCfg sc;
    memset(&sc, 0xFF, sizeof(sc));
    EEPROM.put(CFG_ADDR, sc);
    EEPROM.commit();
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

//Surveillance du capteur de distance (VL53L0X) et heartbeat
    #define VL53L0X_I2C_ADDR_DEFAULT 0x29   // adresse I2C par defaut du VL53L0X
    bool sensorInitialized = false;         // lox.begin() a reussi
    bool lastMeasureOk = true;              // resultat de la derniere mesure (false = valeur aberrante)
    bool sensorOk = false;                  // etat de sante global publie vers HA


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
    #define heartbeat_topic mqtt_topic "/heartbeat"          // uptime en secondes, publie toutes les 60 s (non retenu)
    #define sensorstatus_topic mqtt_topic "/sensorstatus"    // ON = capteur de pellets OK, OFF = capteur en defaut (non retenu)
    #define ip_topic mqtt_topic "/ip"                        // adresse IP de l'ESP, republiee toutes les 60 s (retenue : derniere IP connue)
    #define device_information "{\"manufacturer\": \"Philibert Cheminot\",\"identifiers\": [\"42396f39-80d2-493b-8e8e-31a70e700bc6\"],\"model\": \"Micronova Controller\",\"name\": \"Micronova Controller\",\"sw_version\": \"" FW_VERSION "\"}"


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

//Connect to MQTT server
    void reconnect_mqtt() 
    {
        Write_Log(client, "MQTT : configuration");
        Serial.println();
        client.setServer(cfgMqttHost.c_str(), cfgMqttPort);
        client.setCallback(callback);
        //Loop until we're reconnected
        while (!client.connected())
        {
            bool connected = (cfgMqttUser.length() > 0)
                ? client.connect(MQTT_CLIENT_ID, cfgMqttUser.c_str(), cfgMqttPass.c_str())
                : client.connect(MQTT_CLIENT_ID);   // broker sans authentification

            if (connected)
            {
                client.setBufferSize(1024);
            }
            else
            {
                Serial.println("[MQTT] Connexion impossible (code " + String(client.state()) + "), nouvel essai dans 5 s");
                //Pendant l'attente, la page web reste disponible : on peut corriger la configuration MQTT
                unsigned long waitStart = millis();
                while (millis() - waitStart < 5000)
                {
                    webServer.handleClient();
                    ArduinoOTA.handle();
                    delay(10);
                }
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
            String select_payload = "{\"name\": \"Temperature\", \"command_topic\": \"" cmd_topic "\", \"state_topic\": \"" settemp_topic "\",\"options\":[\"17\",\"17.5\",\"18\",\"18.5\",\"19\",\"19.5\",\"20\",\"20.5\",\"21\"],\"retain\": false, \"optimistic\": false, \"qos\": 0, \"icon\": \"mdi:thermometer\",  \"command_template\": \"T{{ ((value | float) * 2) | int }}\", \"unique_id\": \"d5668e9c-153c-4330-ff53-a5bdfffa4413\", \"device\": " device_information "}";            
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

        //Sensor Heartbeat (uptime en s) : devient indisponible dans HA si plus de message depuis 180 s
        String hb_sensor_topic = "homeassistant/sensor/Micronova/Heartbeat/config";
        String hb_sensor_payload = "{\"name\": \"Heartbeat\", \"state_topic\": \"" heartbeat_topic "\", \"qos\": 0, \"device_class\": \"duration\", \"unit_of_measurement\": \"s\", \"entity_category\": \"diagnostic\", \"expire_after\": 180, \"icon\": \"mdi:heart-pulse\", \"unique_id\": \"3f1c8a52-7b4e-4d19-9a60-1e2b5c7d8f01\",\"device\": " device_information "}";
        client.publish(hb_sensor_topic.c_str(), hb_sensor_payload.c_str(), true);
        client.loop();
        delay(150);

        //Binary sensor Capteur pellets : ON = capteur OK, OFF = defaut, indisponible = ESP injoignable
        String ps_sensor_topic = "homeassistant/binary_sensor/Micronova/PelletSensor/config";
        String ps_sensor_payload = "{\"name\": \"Capteur pellets\", \"state_topic\": \"" sensorstatus_topic "\", \"payload_on\": \"ON\", \"payload_off\": \"OFF\", \"qos\": 0, \"device_class\": \"connectivity\", \"expire_after\": 180, \"icon\": \"mdi:signal-distance-variant\", \"unique_id\": \"a7d94c3e-2b18-4f6a-8c05-9d1e3b4f6a02\",\"device\": " device_information "}";
        client.publish(ps_sensor_topic.c_str(), ps_sensor_payload.c_str(), true);
        client.loop();
        delay(150);

        //Sensor Adresse IP : derniere IP connue (message retenu, pas d'expiration)
        String ip_sensor_topic = "homeassistant/sensor/Micronova/IP/config";
        String ip_sensor_payload = "{\"name\": \"Adresse IP\", \"state_topic\": \"" ip_topic "\", \"qos\": 0, \"entity_category\": \"diagnostic\", \"icon\": \"mdi:ip-network\", \"unique_id\": \"c5e2b7a1-6d3f-4a98-b1c4-8e0f2a9d7b03\",\"device\": " device_information "}";
        client.publish(ip_sensor_topic.c_str(), ip_sensor_payload.c_str(), true);
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
    digitalWrite(pinEnableRx, HIGH);
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

            // Etat du select HA : doit correspondre exactement a une option ("19" ou "19.5")
            {
                int raw = (int)tempSet;   // valeur stockee par le poele = consigne x 2
                String selState = String(raw / 2) + ((raw % 2) ? ".5" : "");
                client.publish(settemp_topic, selState.c_str(), true);
            }
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
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getAmbTemp() //Get room temperature
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(ambTempAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getStovePower() //Get Puissance réelle
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(stovePwrAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getFumeTemp() //Get fume temperature
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(fumeTempAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getBlowTemp() //Get blow temperature
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(blowTempAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getCardTemp() //Get card temperature
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(cardTempAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getExtractor() //Get extractor speed
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(extractorAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getBlowSpeed() //Get blow speed
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(blowSpeedAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getVisPellet() //Get speed vis pellet
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(visPelletAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getFixVacuum() //Get speed vis pellet
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(fixVacuumAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getReadVacuum() //Get speed vis pellet
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(readVacuumAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getMultVacuum() //Get speed vis pellet
{
    const byte readByte = _RAMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(multVacuumAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getTempSet() //Get the thermostat setting
{
    const byte readByte = _EEPROMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(tempSetAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getPowerSet() //Get the Power setting
{
    const byte readByte = _EEPROMR;
    StoveSerial.write(readByte);
    delay(1);
    StoveSerial.write(pwrSetAddr);
    digitalWrite(pinEnableRx, LOW);
    delay(80);
    checkStoveReply();
}

void getQte() //Get speed vis pellet
{
    // Capteur absent ou non initialise : on ne tente pas de mesure
    if (!sensorInitialized)
    {
        lastMeasureOk = false;
        Write_Log(client,"getQte: capteur non initialise, mesure ignoree.");
        return;
    }

    if (measure_sleep==0)
    {
        measure_sleep++;
        VL53L0X_RangingMeasurementData_t measure;        
        lox.rangingTest(&measure, false);
        
        distance=measure.RangeMilliMeter;
        if (distance>1000)
        {
            count_reboot++;
            lastMeasureOk = false;   // valeur aberrante : capteur en defaut
            Write_Log(client,"getQte: Measure Error. NB reboots="+ String(count_reboot));        
        }
        else
        {            
            if (measure.RangeStatus != 4)
            {
                count_reboot=0;
                
                lastMeasureOk = true;    // mesure valide
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

 

//Teste la presence du capteur sur le bus I2C (detecte un fil debranche sans faire de mesure)
bool sensorPresentI2C()
{
    Wire.beginTransmission(VL53L0X_I2C_ADDR_DEFAULT);
    return (Wire.endTransmission() == 0);
}

//Met a jour l'etat de sante du capteur (appele toutes les 60 s) et tente de le reinitialiser s'il revient
void updateSensorHealth()
{
    bool present = sensorPresentI2C();

    if (!present)
    {
        if (sensorInitialized) Write_Log(client,"Sensor : VL53L0X absent du bus I2C.");
        sensorInitialized = false;   // sera reinitialise des qu'il reapparait
    }
    else if (!sensorInitialized)
    {
        if (lox.begin())
        {
            sensorInitialized = true;
            lastMeasureOk = true;
            Write_Log(client,"Sensor : VL53L0X (re)initialise.");
        }
        else
        {
            Write_Log(client,"Sensor : VL53L0X present mais init KO.");
        }
    }

    sensorOk = present && sensorInitialized && lastMeasureOk;
}

//Publie le heartbeat (uptime en s) et l'etat du capteur. Messages NON retenus : un message
//retenu serait relivre a HA apres un redemarrage et ferait croire que l'ESP est vivant.
//L'adresse IP, elle, est retenue : on veut retrouver la derniere IP connue meme si l'ESP est eteint.
void publishHeartbeat()
{
    client.publish(heartbeat_topic, String(millis() / 1000UL).c_str(), false);
    client.publish(sensorstatus_topic, sensorOk ? "ON" : "OFF", false);
    client.publish(ip_topic, WiFi.localIP().toString().c_str(), true);
}

//Mode alignement du capteur : mesures en continu pendant N secondes (non bloquant).
//Le VL53L0X n'emet son infrarouge (940 nm) que pendant une mesure : en mesurant en boucle,
//le point reste visible a la camera d'un telephone, et la distance s'affiche en direct sur la page web.
    #define ALIGN_DEFAULT_S 10
    #define ALIGN_MAX_S 120
    bool alignActive = false;
    unsigned long alignEnd = 0;
    int alignDistance = 0;
    int alignStatus = -1;        // RangeStatus du VL53L0X : 0 = mesure valide, 4 = pas de retour, -1 = aucune mesure

//Appelee a chaque tour de loop() : une mesure tant que le mode alignement est actif
void alignTick()
{
    if (!alignActive) return;

    if ((long)(millis() - alignEnd) >= 0)
    {
        alignActive = false;
        Write_Log(client, "Align : mode alignement termine.");
        return;
    }

    VL53L0X_RangingMeasurementData_t m;
    lox.rangingTest(&m, false);
    alignDistance = m.RangeMilliMeter;
    alignStatus = m.RangeStatus;
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

//Interface web (inspiree de watermeter2mqtt) : page statique en PROGMEM + API JSON /api/status.
//La page se rafraichit toute seule (toutes les 5 s, toutes les 0,5 s pendant l'alignement) et les actions
//(hauteur, alignement, redemarrage, mise a jour OTA) passent par fetch/XHR sans recharger la page.
//Toutes les pages sont protegees par les identifiants saisis a la configuration (utilisateur / mot de passe web).

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Poêle Micronova</title>
<style>
  :root { --bg:#f3f5f8; --card:#fff; --txt:#1c2530; --mut:#6b7785; --acc:#1f7ae0; --ok:#1e9e5a; --ko:#d64545; --bd:#e1e6ec; }
  @media (prefers-color-scheme: dark) {
    :root { --bg:#12171d; --card:#1b222b; --txt:#e8edf2; --mut:#93a0ae; --bd:#2a3440; }
  }
  * { box-sizing:border-box; }
  body { margin:0; font-family:system-ui,sans-serif; background:var(--bg); color:var(--txt); }
  main { max-width:720px; margin:0 auto; padding:16px; }
  h1 { font-size:1.4rem; margin:8px 0 16px; }
  h2 { font-size:1.05rem; margin:0 0 12px; }
  .card { background:var(--card); border:1px solid var(--bd); border-radius:12px; padding:16px; margin-bottom:16px; }
  .grid { display:grid; grid-template-columns:1fr 1fr; gap:12px; }
  .item span { display:block; color:var(--mut); font-size:.8rem; }
  .item b { font-size:1.05rem; word-break:break-all; }
  .big b { font-size:1.8rem; }
  .hint { color:var(--mut); font-size:.85rem; margin:0 0 12px; }
  .chk { display:flex; align-items:center; gap:8px; margin:6px 0 10px; color:var(--txt); font-size:.95rem; }
  .chk input { width:auto; }
  label { display:block; margin:10px 0 4px; font-size:.85rem; color:var(--mut); }
  input, select { width:100%; padding:10px; border:1px solid var(--bd); border-radius:8px; background:var(--bg); color:var(--txt); font-size:1rem; }
  button { padding:10px 16px; border:0; border-radius:8px; font-size:1rem; cursor:pointer; background:var(--acc); color:#fff; }
  button:disabled { opacity:.5; cursor:default; }
  button.danger { background:var(--ko); }
  .row { display:flex; gap:8px; margin-top:14px; flex-wrap:wrap; }
  .dot { display:inline-block; width:10px; height:10px; border-radius:50%; margin-right:6px; background:var(--ko); }
  .dot.on { background:var(--ok); }
  .ok { color:var(--ok); } .ko { color:var(--ko); }
  .msg { margin-top:10px; font-size:.9rem; color:var(--mut); }
  .msg.ko { color:var(--ko); }
  button.sec { background:var(--bd); color:var(--txt); }
  .two { display:grid; grid-template-columns:1fr 1fr; gap:12px; }
  .tabs { display:flex; gap:8px; margin:0 0 16px; }
  .tab { flex:1; background:var(--card); color:var(--mut); border:1px solid var(--bd); }
  .tab.on { background:var(--acc); color:#fff; border-color:var(--acc); }
  @media (max-width:480px) { .grid, .two { grid-template-columns:1fr; } }
</style>
</head>
<body>
<main>
  <h1>🔥 Contrôleur poêle Micronova</h1>

  <nav class="tabs" id="tabs">
    <button type="button" class="tab on" id="tabdash">Tableau de bord</button>
    <button type="button" class="tab" id="tabcfg">Configuration</button>
  </nav>

  <div id="pdash">

  <div class="card">
    <h2>Poêle</h2>
    <div class="grid">
      <div class="item big"><span>Température</span><b id="temp">–</b></div>
      <div class="item big"><span>Consigne</span><b id="consigne">–</b></div>
    </div>
    <div class="item" style="margin-top:12px"><span>État du poêle</span><b id="state">–</b></div>
  </div>

  <div class="card">
    <h2>Pellets</h2>
    <div class="grid">
      <div class="item big"><span>Quantité restante</span><b id="qte">–</b></div>
      <div class="item big"><span>Hauteur lue</span><b id="dist">–</b></div>
    </div>
    <form id="hform">
      <label for="hin">Hauteur de la cuve (mm)</label>
      <input id="hin" name="hauteur" type="number" min="101" max="1999" required>
      <div class="row"><button type="submit">Enregistrer</button></div>
    </form>
    <div class="msg" id="hmsg"></div>
  </div>

  <div class="card">
    <h2>Alignement du capteur</h2>
    <p class="hint">Lance des mesures en continu : le point infrarouge reste visible à la caméra d'un téléphone (caméra frontale de préférence) et la distance s'affiche en direct ci-dessous.</p>
    <div class="grid">
      <div class="item big"><span>Distance</span><b id="aldist">–</b></div>
      <div class="item"><span>État</span><b id="alstate">–</b></div>
    </div>
    <div class="item" style="margin-top:12px"><span>Qualité de la mesure</span><b id="alq">–</b></div>
    <label for="aldur">Durée</label>
    <select id="aldur">
      <option value="10">10 s</option>
      <option value="30">30 s</option>
      <option value="60">60 s</option>
      <option value="120">120 s</option>
    </select>
    <div class="row"><button id="alstart">Lancer l'alignement</button></div>
    <div class="msg" id="almsg"></div>
  </div>

  <div class="card">
    <h2>Système</h2>
    <div class="grid">
      <div class="item"><span>Adresse IP</span><b id="ip">–</b></div>
      <div class="item"><span>Hostname</span><b id="host">–</b></div>
      <div class="item"><span>Wi-Fi</span><b id="ssid">–</b></div>
      <div class="item"><span>Signal</span><b id="rssi">–</b></div>
      <div class="item"><span>Adresse MAC</span><b id="mac">–</b></div>
      <div class="item"><span>MQTT</span><b><i class="dot" id="mqttdot"></i><span id="mqtt" style="display:inline;font-size:inherit;color:inherit">–</span></b></div>
      <div class="item"><span>Capteur pellets (VL53L0X)</span><b><i class="dot" id="sensdot"></i><span id="sens" style="display:inline;font-size:inherit;color:inherit">–</span></b></div>
      <div class="item"><span>Uptime</span><b id="uptime">–</b></div>
      <div class="item"><span>Dernier reset</span><b id="reset">–</b></div>
      <div class="item"><span>Mémoire libre</span><b id="heap">–</b></div>
      <div class="item"><span>Version du firmware</span><b id="version">–</b></div>
    </div>
  </div>

  <div class="card">
    <h2>Mise à jour du firmware (OTA)</h2>
    <form id="fwForm">
      <label for="fwFile">Fichier .bin (Croquis > Exporter les binaires compilés)</label>
      <input id="fwFile" type="file" accept=".bin" required>
      <div class="row">
        <button type="submit" id="fwBtn">Envoyer et installer</button>
      </div>
    </form>
    <div class="msg" id="fwmsg"></div>
  </div>

  <div class="card">
    <h2>Maintenance</h2>
    <div class="row" style="margin-top:0">
      <button class="danger" id="reboot">Redémarrer l'ESP</button>
    </div>
    <div class="msg" id="sysmsg"></div>
  </div>
  </div>

  <div id="pcfg" style="display:none">
    <p class="hint" id="c_intro"></p>

    <form id="c_cfg" autocomplete="off">
    <div class="card">
      <h2>Wi-Fi</h2>
      <label for="c_ssid">Réseau Wi-Fi (SSID)</label>
      <input id="c_ssid" name="ssid" list="c_nets" maxlength="32" required>
      <datalist id="c_nets"></datalist>
      <label for="c_pass">Mot de passe Wi-Fi (8 à 63 caractères, vide si le réseau est ouvert)</label>
      <input id="c_pass" name="pass" type="password" maxlength="63" autocomplete="new-password">
      <label for="c_hostname">Hostname (lettres, chiffres, tirets)</label>
      <input id="c_hostname" name="hostname" pattern="[A-Za-z0-9\-]{1,32}" maxlength="32" required>
      <div class="row"><button type="button" class="sec" id="c_scan">Rechercher les réseaux</button></div>
      <div class="msg" id="c_scanmsg"></div>
    </div>

    <div class="card">
      <h2>Serveur MQTT</h2>
      <div class="two">
        <div><label for="c_mqttHost">Adresse du serveur</label><input id="c_mqttHost" name="mqttHost" maxlength="64" required></div>
        <div><label for="c_mqttPort">Port</label><input id="c_mqttPort" name="mqttPort" type="number" min="1" max="65535" required></div>
        <div><label for="c_mqttUser">Utilisateur (vide si aucun)</label><input id="c_mqttUser" name="mqttUser" maxlength="32" autocomplete="off"></div>
        <div><label for="c_mqttPass">Mot de passe</label><input id="c_mqttPass" name="mqttPass" type="password" maxlength="63" autocomplete="new-password"></div>
      </div>
    </div>

    <div class="card">
      <h2>Broches</h2>
      <p class="hint">Numéros D1 à D8 d'une carte NodeMCU ou Wemos D1 mini. Les valeurs par défaut sont celles du montage d'origine. Les broches D3, D4 et D8 (GPIO0, 2, 15) conditionnent le démarrage de l'ESP. Chaque fonction doit avoir sa propre broche.</p>
      <div class="two">
        <div><label for="c_pinRx">Liaison poêle : réception (RX)</label><select id="c_pinRx" name="pinRx"></select></div>
        <div><label for="c_pinTx">Liaison poêle : émission (TX)</label><select id="c_pinTx" name="pinTx"></select></div>
        <div><label for="c_pinEn">Activation de la réception (optocoupleur)</label><select id="c_pinEn" name="pinEn"></select></div>
        <div></div>
        <div><label for="c_pinSda">Capteur VL53L0X : SDA</label><select id="c_pinSda" name="pinSda"></select></div>
        <div><label for="c_pinScl">Capteur VL53L0X : SCL</label><select id="c_pinScl" name="pinScl"></select></div>
      </div>
    </div>

    <div class="card">
      <h2>Cuve à pellets</h2>
      <label for="c_hauteur">Hauteur de la cuve (mm, de 101 à 1999)</label>
      <input id="c_hauteur" name="hauteur" type="number" min="101" max="1999" required>
    </div>

    <div class="card">
      <h2>Accès à cette page et mises à jour</h2>
      <label class="chk"><input type="checkbox" id="c_webAuth" name="webAuth" value="1" checked> Protéger l'accès par un identifiant et un mot de passe</label>
      <p class="hint" id="c_authhint">Ces identifiants protègent la page web, la mise à jour par navigateur (/update) et la mise à jour depuis l'IDE Arduino.</p>
      <div class="two" id="c_authfields">
        <div><label for="c_webUser">Utilisateur</label><input id="c_webUser" name="webUser" maxlength="32" autocomplete="off"></div>
        <div></div>
        <div><label for="c_webPass">Mot de passe (6 caractères minimum)</label><input id="c_webPass" name="webPass" type="password" maxlength="63" autocomplete="new-password"></div>
        <div><label for="c_webPass2">Confirmer le mot de passe</label><input id="c_webPass2" name="webPass2" type="password" maxlength="63" autocomplete="new-password"></div>
      </div>
    </div>

    <div class="row">
      <button type="submit" id="c_save">Enregistrer et redémarrer</button>
      <button type="button" class="sec" id="c_cancel">Annuler</button>
    </div>
    <div class="msg" id="c_msg"></div>
  </form>

    <div class="card" id="c_resetcard" style="margin-top:16px">
    <h2>Configuration initiale</h2>
    <p class="hint">Efface le Wi-Fi, le serveur MQTT, les broches et les identifiants, puis redémarre en mode configuration (point d'accès « Micronova-Setup »).</p>
    <button type="button" class="danger" id="c_reset">Revenir à la configuration initiale</button>
    <div class="msg" id="c_resetmsg"></div>
  </div>
  </div>
</main>

<script>
const $ = id => document.getElementById(id);
let formFilled = false;
let timer = null;

function fmtUptime(s) {
  const d = Math.floor(s/86400), h = Math.floor(s%86400/3600), m = Math.floor(s%3600/60);
  return (d ? d+' j ' : '') + h + ' h ' + m + ' min';
}

function fmtC(v) {
  const n = parseFloat(v);
  return isNaN(n) ? '–' : n.toFixed(1) + ' °C';
}

async function post(url, params) {
  const r = await fetch(url, { method:'POST', body:new URLSearchParams(params || {}) });
  return { ok:r.ok, text:await r.text() };
}

async function refresh() {
  let next = 5000;
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    $('temp').textContent     = fmtC(s.temperature);
    $('consigne').textContent = fmtC(s.consigne);
    $('state').textContent    = s.state;
    $('qte').textContent      = s.qte < 0 ? '–' : s.qte.toFixed(1) + ' %';
    $('dist').textContent     = s.distance > 0 ? s.distance + ' mm' : '–';

    $('aldist').textContent  = s.alignStatus < 0 ? '–' : s.alignDistance + ' mm';
    $('alstate').textContent = s.alignActive ? 'Infrarouge actif : ' + s.alignRemain + ' s' : 'Arrêté';
    let q = '–';
    if (s.alignStatus === 0) q = 'Mesure valide';
    else if (s.alignStatus === 4) q = 'Pas de retour (cible hors portée, trop sombre ou mal orientée)';
    else if (s.alignStatus > 0) q = 'Mesure douteuse (code ' + s.alignStatus + ')';
    $('alq').textContent = q;
    $('alq').className = s.alignStatus === 0 ? 'ok' : (s.alignStatus > 0 ? 'ko' : '');
    $('alstart').disabled = s.alignActive;
    if (s.alignActive) next = 500;   // suivi en direct pendant l'alignement

    $('ip').textContent    = s.ip;
    $('host').textContent  = s.hostname + '.local';
    $('ssid').textContent  = s.ssid;
    $('rssi').textContent  = s.rssi + ' dBm';
    $('mac').textContent   = s.mac;
    $('mqtt').textContent  = s.mqtt ? 'Connecté' : 'Déconnecté';
    $('mqttdot').className = 'dot' + (s.mqtt ? ' on' : '');
    $('sens').textContent  = s.sensorOk ? 'OK' : 'En défaut';
    $('sensdot').className = 'dot' + (s.sensorOk ? ' on' : '');
    $('uptime').textContent  = fmtUptime(s.uptime);
    $('reset').textContent   = s.resetReason;
    $('heap').textContent    = s.heap + ' ko (fragmentation ' + s.frag + ' %)';
    $('version').textContent = s.version + ' (' + s.build + ')';
    if (!formFilled) {
      $('hin').value = s.hauteur;
      formFilled = true;
    }
  } catch (e) { /* ESP injoignable (redémarrage ?) */ }
  clearTimeout(timer);
  timer = setTimeout(refresh, next);
}

$('hform').onsubmit = async ev => {
  ev.preventDefault();
  try { $('hmsg').textContent = (await post('/set', { hauteur: $('hin').value })).text; }
  catch (e) { $('hmsg').textContent = 'Erreur lors de l\'envoi.'; }
  refresh();
};

$('alstart').onclick = async () => {
  try { $('almsg').textContent = (await post('/align/start', { duree: $('aldur').value })).text; }
  catch (e) { $('almsg').textContent = 'Erreur lors de l\'envoi.'; }
  refresh();
};

$('fwForm').onsubmit = ev => {
  ev.preventDefault();
  const f = $('fwFile').files[0];
  if (!f) return;
  if (!confirm('Installer ' + f.name + ' ? L\'ESP redémarrera ensuite.')) return;
  const x = new XMLHttpRequest();
  x.open('POST', '/update');
  x.upload.onprogress = e => {
    if (e.lengthComputable) $('fwmsg').textContent = 'Envoi : ' + Math.round(e.loaded * 100 / e.total) + ' %';
  };
  x.onload = () => {
    if (x.status === 200) {
      $('fwmsg').textContent = 'Mise à jour installée. Redémarrage… la page se rechargera dans 20 s.';
      setTimeout(() => location.reload(), 20000);
    } else {
      $('fwmsg').textContent = 'Échec de la mise à jour : ' + x.responseText;
      $('fwBtn').disabled = false;
    }
  };
  x.onerror = () => { $('fwmsg').textContent = 'Erreur réseau pendant l\'envoi.'; $('fwBtn').disabled = false; };
  $('fwBtn').disabled = true;
  const fd = new FormData();
  fd.append('update', f, f.name);
  x.send(fd);
};

$('reboot').onclick = async () => {
  if (!confirm('Redémarrer l\'ESP ?')) return;
  try { await fetch('/reboot', { method:'POST' }); } catch (e) {}
  $('sysmsg').textContent = 'Redémarrage en cours… la page se rechargera dans 15 s.';
  setTimeout(() => location.reload(), 15000);
};

// ---------------- Onglets et configuration ----------------
const PINS = [[5,'D1'],[4,'D2'],[0,'D3'],[2,'D4'],[14,'D5'],[12,'D6'],[13,'D7'],[15,'D8']];
const PIN_FIELDS = ['c_pinRx','c_pinTx','c_pinEn','c_pinSda','c_pinScl'];
let firstSetup = true;   // aucune configuration stockee : seul l'onglet Configuration est proposé
let hadWebPass = false;  // un mot de passe web est deja enregistre (laisse vide = conserve)
let portal = false;      // point d'accès de configuration : pas de tableau de bord (poêle et MQTT inactifs)

PIN_FIELDS.forEach(id => {
  $(id).innerHTML = PINS.map(p => '<option value="' + p[0] + '">' + p[1] + ' (GPIO' + p[0] + ')</option>').join('');
});

function showTab(name) {
  $('pdash').style.display = name === 'dash' ? '' : 'none';
  $('pcfg').style.display  = name === 'cfg' ? '' : 'none';
  $('tabdash').className = 'tab' + (name === 'dash' ? ' on' : '');
  $('tabcfg').className  = 'tab' + (name === 'cfg' ? ' on' : '');
}
$('tabdash').onclick = () => showTab('dash');
$('tabcfg').onclick  = () => showTab('cfg');

function updateAuthUi() {
  const on = $('c_webAuth').checked;
  $('c_authfields').style.display = on ? '' : 'none';
  $('c_authhint').textContent = on
    ? 'Ces identifiants protègent la page web, la mise à jour par navigateur (/update) et la mise à jour depuis l\'IDE Arduino.'
    : 'Sans identifiant, toute personne présente sur votre réseau peut modifier la configuration et mettre à jour le firmware.';
  const needPass = on && !hadWebPass;
  $('c_webUser').required = on;
  $('c_webPass').required = needPass;
  $('c_webPass2').required = needPass;
}
$('c_webAuth').onchange = updateAuthUi;

function show(id, text, bad) {
  $(id).textContent = text;
  $(id).className = 'msg' + (bad ? ' ko' : '');
}

function fillNets(list) {
  $('c_nets').innerHTML = list.map(n => '<option value="' + n.ssid.replace(/"/g, '&quot;') + '">' + n.rssi + ' dBm</option>').join('');
  return list.length;
}

async function loadConfig() {
  try {
    const c = await (await fetch('/api/config')).json();
    firstSetup = c.first;
    portal = c.portal;
    $('c_ssid').value = c.ssid;
    $('c_hostname').value = c.hostname;
    $('c_mqttHost').value = c.mqttHost;
    $('c_mqttPort').value = c.mqttPort;
    $('c_mqttUser').value = c.mqttUser;
    $('c_pinRx').value = c.pinRx;
    $('c_pinTx').value = c.pinTx;
    $('c_pinEn').value = c.pinEn;
    $('c_pinSda').value = c.pinSda;
    $('c_pinScl').value = c.pinScl;
    $('c_hauteur').value = c.hauteur;
    $('c_webUser').value = c.webUser;
    $('c_pass').value = '';  $('c_mqttPass').value = '';  $('c_webPass').value = '';  $('c_webPass2').value = '';
    $('c_pass').placeholder = c.hasWifiPass ? '(inchangé)' : '';
    $('c_mqttPass').placeholder = c.hasMqttPass ? '(inchangé)' : '';
    hadWebPass = !firstSetup && c.webAuth;
    $('c_webAuth').checked = firstSetup ? true : c.webAuth;   // protege par defaut a la premiere configuration
    $('c_webPass').placeholder = hadWebPass ? '(inchangé)' : '';
    updateAuthUi();
    $('c_resetcard').style.display = firstSetup ? 'none' : '';
    if (firstSetup) {
      $('c_intro').textContent = 'Première configuration : renseignez le Wi-Fi, le serveur MQTT, les broches et la hauteur de la cuve (le compte d\'accès à la page est facultatif). Après l\'enregistrement, l\'ESP redémarre et utilise ces valeurs.';
    } else if (portal) {
      $('c_intro').textContent = 'L\'ESP n\'a pas réussi à se connecter au Wi-Fi « ' + c.ssid + ' ». Vérifiez les réglages ci-dessous, puis enregistrez.';
    } else {
      $('c_intro').textContent = 'Les modifications sont appliquées après le redémarrage de l\'ESP. Un mot de passe laissé vide est conservé.';
    }
  } catch (e) { show('c_msg', 'Impossible de lire la configuration actuelle.', true); }
}

$('c_scan').onclick = async () => {
  show('c_scanmsg', 'Recherche en cours… (la connexion peut être brièvement interrompue)');
  try {
    const n = fillNets(await (await fetch('/api/scan?refresh=1')).json());
    show('c_scanmsg', n + ' réseau(x) trouvé(s). Videz le champ « Réseau Wi-Fi » puis cliquez dedans pour afficher la liste.');
  } catch (e) { show('c_scanmsg', 'Échec de la recherche.', true); }
};

$('c_cancel').onclick = () => { show('c_msg', ''); loadConfig(); };

$('c_cfg').onsubmit = async ev => {
  ev.preventDefault();
  const auth = $('c_webAuth').checked;
  if (auth && $('c_webPass').value !== $('c_webPass2').value) { show('c_msg', 'Les deux mots de passe web ne sont pas identiques.', true); return; }
  if (new Set(PIN_FIELDS.map(id => $(id).value)).size !== PIN_FIELDS.length) { show('c_msg', 'Chaque fonction doit utiliser une broche différente.', true); return; }
  if (!auth && !confirm('Aucun identifiant ne protégera la page, /update et l\'OTA : toute personne présente sur votre réseau pourra modifier la configuration et mettre à jour le firmware. Continuer ?')) return;
  const body = new URLSearchParams(new FormData($('c_cfg')));
  body.delete('webPass2');
  $('c_save').disabled = true;
  try {
    const r = await fetch('/api/config', { method:'POST', body });
    show('c_msg', await r.text(), !r.ok);
    if (r.ok) { if (!portal) setTimeout(() => location.href = '/', 15000); }
    else $('c_save').disabled = false;
  } catch (e) { show('c_msg', 'Connexion perdue : l\'ESP est probablement en train de redémarrer.'); }
};

$('c_reset').onclick = async () => {
  if (!confirm('Effacer la configuration (Wi-Fi, MQTT, broches, identifiants) et redémarrer en mode configuration ?')) return;
  try { show('c_resetmsg', await (await fetch('/config/reset', { method:'POST' })).text()); }
  catch (e) { show('c_resetmsg', 'Redémarrage en cours…'); }
};

async function init() {
  await loadConfig();
  if (portal) {
    // point d'accès de configuration : uniquement l'onglet Configuration
    $('tabs').style.display = 'none';
    showTab('cfg');
    try { fillNets(await (await fetch('/api/scan')).json()); } catch (e) {}
    setInterval(() => { fetch('/api/config').catch(() => {}); }, 60000);   // evite le nouvel essai automatique pendant la saisie
  } else {
    showTab(location.pathname === '/config' || location.hash === '#config' ? 'cfg' : 'dash');
    refresh();   // pas de recherche de reseaux automatique ici : elle perturbe brievement le Wi-Fi (bouton « Rechercher »)
  }
}

init();
</script>
</body>
</html>
)rawliteral";

//Echappe une chaine pour l'inclure dans du JSON
String jsonEscape(const String &in)
{
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++)
    {
        char c = in[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') {}
        else out += c;
    }
    return out;
}

//Le compte web est facultatif : sans mot de passe enregistre, la page, /update et l'OTA sont en acces libre.
bool webAuthEnabled()
{
    return cfgWebPass.length() > 0;
}

//Authentification HTTP basique : identifiants saisis a la configuration (les memes pour la page, /update et l'OTA),
//sauf si l'utilisateur a choisi de ne pas en definir (acces libre).
//Pendant la premiere configuration, aucun identifiant n'existe encore : seule la page de configuration est servie, sans mot de passe.
bool checkAuth()
{
    lastWebActivity = millis();
    if (portalFirstSetup || !webAuthEnabled()) return true;
    if (!webServer.authenticate(cfgWebUser.c_str(), cfgWebPass.c_str()))
    {
        webServer.requestAuthentication();
        return false;
    }
    return true;
}

//Redirige vers la page de configuration (portail captif : toute adresse tapee renvoie ici)
void redirectToConfig()
{
    lastWebActivity = millis();
    webServer.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/config", true);
    webServer.send(302, "text/plain", "");
}

//Handlers HTTP
void handleRoot()
{
    if (!checkAuth()) return;
    webServer.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

//GET /api/status : tout l'etat en JSON, interroge par la page
void handleStatus()
{
    if (!checkAuth()) return;

    long alignRemain = alignActive ? (long)(alignEnd - millis()) / 1000L + 1 : 0;
    if (alignRemain < 0) alignRemain = 0;

    String j = "{";
    j += "\"temperature\":\"" + jsonEscape(temperature) + "\"";
    j += ",\"consigne\":\"" + jsonEscape(consigne) + "\"";
    j += ",\"state\":\"" + jsonEscape(stove_state) + "\"";
    j += ",\"distance\":" + String(distance);
    j += ",\"qte\":" + String(lastQte, 1);
    j += ",\"hauteur\":" + String(hauteur_cuve);
    j += ",\"alignActive\":" + String(alignActive ? "true" : "false");
    j += ",\"alignRemain\":" + String(alignRemain);
    j += ",\"alignDistance\":" + String(alignDistance);
    j += ",\"alignStatus\":" + String(alignStatus);
    j += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    j += ",\"hostname\":\"" + jsonEscape(espHostname) + "\"";
    j += ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"";
    j += ",\"rssi\":" + String(WiFi.RSSI());
    j += ",\"mac\":\"" + WiFi.macAddress() + "\"";
    j += ",\"mqtt\":" + String(client.connected() ? "true" : "false");
    j += ",\"sensorOk\":" + String(sensorOk ? "true" : "false");
    j += ",\"uptime\":" + String(millis() / 1000UL);
    j += ",\"resetReason\":\"" + jsonEscape(ESP.getResetReason()) + "\"";
    j += ",\"heap\":" + String(ESP.getFreeHeap() / 1024.0, 1);
    j += ",\"frag\":" + String(ESP.getHeapFragmentation());
    j += ",\"version\":\"" FW_VERSION "\"";
    j += ",\"build\":\"" __DATE__ " " __TIME__ "\"";
    j += "}";

    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/json", j);
}

//POST /set : hauteur de la cuve (enregistree en EEPROM)
void handleSet()
{
    if (!checkAuth()) return;
    Serial.println("HTTP hauteur appelé");

    int newHauteur = webServer.arg("hauteur").toInt();
    if (newHauteur > 100 && newHauteur < 2000) // sécurité
    {
        hauteur_cuve = newHauteur;
        hauteur_set = true;
        saveHauteur(hauteur_cuve);   // conservee apres un redemarrage

        Write_Log(client, "WEB : hauteur_cuve mise à jour = " + String(hauteur_cuve));
        webServer.send(200, "text/plain; charset=utf-8", "Hauteur enregistrée : " + String(hauteur_cuve) + " mm.");
    }
    else
    {
        webServer.send(400, "text/plain; charset=utf-8", "Hauteur invalide (101 à 1999 mm).");
    }
}

//POST /reboot
void handleReboot()
{
    if (!checkAuth()) return;
    Serial.println("HTTP reboot appelé");
    webServer.send(200, "text/plain; charset=utf-8", "Redémarrage…");
    Write_Log(client, "WEB : Redémarrage ESP demandé");
    delay(500);   // laisse le temps d'envoyer la reponse
    ESP.restart();
}

//POST /align/start (champ "duree" en secondes) : demarre le mode alignement
void handleAlignStart()
{
    if (!checkAuth()) return;
    Serial.println("HTTP align/start appele");

    if (!sensorInitialized)
    {
        webServer.send(503, "text/plain; charset=utf-8", "Capteur VL53L0X non disponible : vérifier le câblage (SDA/SCL, alimentation).");
        return;
    }

    int duree = ALIGN_DEFAULT_S;
    if (webServer.hasArg("duree")) duree = webServer.arg("duree").toInt();
    if (duree < 5) duree = ALIGN_DEFAULT_S;
    if (duree > ALIGN_MAX_S) duree = ALIGN_MAX_S;

    alignStatus = -1;
    alignEnd = millis() + (unsigned long)duree * 1000UL;
    alignActive = true;
    Write_Log(client, "Align : mode alignement demarre pour " + String(duree) + " s.");

    webServer.send(200, "text/plain; charset=utf-8", "Alignement lancé pour " + String(duree) + " s.");
}

//==================== CONFIGURATION (onglet « Configuration » de la page principale) ====================
//Premier demarrage (aucune configuration en EEPROM) : l'ESP ouvre le point d'acces Wi-Fi "Micronova-Setup" et
//sert la meme page, limitee a l'onglet Configuration (sans mot de passe, puisqu'aucun n'existe encore). Apres
//l'enregistrement, il redemarre et utilise les valeurs saisies. Si la connexion au Wi-Fi saisi echoue, le point
//d'acces reapparait (page protegee par le mot de passe web) et l'ESP retente la connexion apres 5 min d'inactivite.
//Une fois connecte, le meme formulaire est disponible dans l'onglet Configuration (/config ouvre directement cet onglet).

//Hors mode configuration : 404 ; en mode configuration : tout renvoie vers la page (portail captif)
void handleNotFound()
{
    if (portalMode)
    {
        redirectToConfig();
        return;
    }
    webServer.send(404, "text/plain", "Not found");
}

//Cherche les reseaux Wi-Fi et renvoie la liste en JSON (SSID masques ignores)
String scanNetworksJson()
{
    int n = WiFi.scanNetworks();
    String j = "[";
    bool first = true;
    for (int i = 0; i < n; i++)
    {
        if (WiFi.SSID(i).length() == 0) continue;
        if (!first) j += ",";
        first = false;
        j += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    j += "]";
    WiFi.scanDelete();
    return j;
}

//GET /api/scan : liste des reseaux (celle du demarrage ; ?refresh=1 relance une recherche)
void handleScan()
{
    if (!checkAuth()) return;
    if (webServer.hasArg("refresh") || scanCache.length() < 3) scanCache = scanNetworksJson();
    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/json", scanCache);
}

//GET /config : meme page que /, ouverte sur l'onglet Configuration
void handleConfigPage()
{
    if (!checkAuth()) return;
    webServer.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

//GET /api/config : valeurs actuelles (les mots de passe ne sont jamais renvoyes)
void handleConfigGet()
{
    if (!checkAuth()) return;

    String j = "{";
    j += "\"first\":" + String(portalFirstSetup ? "true" : "false");
    j += ",\"portal\":" + String(portalMode ? "true" : "false");
    j += ",\"wifiFailed\":" + String((portalMode && !portalFirstSetup) ? "true" : "false");
    j += ",\"ssid\":\"" + jsonEscape(cfgSsid) + "\"";
    j += ",\"hostname\":\"" + jsonEscape(espHostname) + "\"";
    j += ",\"hasWifiPass\":" + String(cfgPass.length() > 0 ? "true" : "false");
    j += ",\"mqttHost\":\"" + jsonEscape(cfgMqttHost) + "\"";
    j += ",\"mqttPort\":" + String(cfgMqttPort);
    j += ",\"mqttUser\":\"" + jsonEscape(cfgMqttUser) + "\"";
    j += ",\"hasMqttPass\":" + String(cfgMqttPass.length() > 0 ? "true" : "false");
    j += ",\"pinRx\":" + String(pinRx);
    j += ",\"pinTx\":" + String(pinTx);
    j += ",\"pinEn\":" + String(pinEnableRx);
    j += ",\"pinSda\":" + String(pinSda);
    j += ",\"pinScl\":" + String(pinScl);
    j += ",\"hauteur\":" + String(hauteur_cuve);
    j += ",\"webAuth\":" + String(webAuthEnabled() ? "true" : "false");
    j += ",\"webUser\":\"" + jsonEscape(cfgWebUser) + "\"";
    j += "}";

    webServer.sendHeader("Cache-Control", "no-store");
    webServer.send(200, "application/json", j);
}

//POST /api/config : valide, enregistre en EEPROM puis redemarre pour appliquer
void handleConfigSave()
{
    if (!checkAuth()) return;
    Serial.println("HTTP config appele");

    String ssid = webServer.arg("ssid");        ssid.trim();
    String pass = webServer.arg("pass");
    String host = webServer.arg("hostname");    host.trim();
    String mHost = webServer.arg("mqttHost");   mHost.trim();
    long mPort = webServer.arg("mqttPort").toInt();
    String mUser = webServer.arg("mqttUser");   mUser.trim();
    String mPass = webServer.arg("mqttPass");
    int pRx = webServer.arg("pinRx").toInt();
    int pTx = webServer.arg("pinTx").toInt();
    int pEn = webServer.arg("pinEn").toInt();
    int pSda = webServer.arg("pinSda").toInt();
    int pScl = webServer.arg("pinScl").toInt();
    int hauteur = webServer.arg("hauteur").toInt();
    bool webAuth = webServer.arg("webAuth") == "1";   // case cochee : compte web active (facultatif)
    String wUser = webServer.arg("webUser");    wUser.trim();
    String wPass = webServer.arg("webPass");
    if (!webAuth)
    {
        wUser = "";   // acces libre : aucun identifiant enregistre
        wPass = "";
    }

    //Champ de mot de passe laisse vide : on garde l'ancien (tant que le reseau / l'utilisateur ne change pas)
    if (cfgConfigured)
    {
        if (pass.length() == 0 && ssid == cfgSsid) pass = cfgPass;
        if (mPass.length() == 0 && mUser == cfgMqttUser) mPass = cfgMqttPass;
        if (webAuth && wPass.length() == 0) wPass = cfgWebPass;
    }

    String err = "";
    if (ssid.length() < 1 || ssid.length() > 32) err = "Nom de réseau Wi-Fi invalide (1 à 32 caractères).";
    else if (pass.length() > 63 || (pass.length() > 0 && pass.length() < 8)) err = "Mot de passe Wi-Fi invalide (8 à 63 caractères, ou vide pour un réseau ouvert).";
    else if (!isValidHostname(host)) err = "Hostname invalide (lettres, chiffres et tirets, 32 max).";
    else if (mHost.length() < 1 || mHost.length() > 64 || mHost.indexOf(' ') >= 0 || mHost.indexOf('/') >= 0) err = "Adresse du serveur MQTT invalide.";
    else if (mPort < 1 || mPort > 65535) err = "Port MQTT invalide (1 à 65535).";
    else if (mUser.length() > 32 || mPass.length() > 63) err = "Identifiants MQTT trop longs.";
    else if (!isValidPin(pRx) || !isValidPin(pTx) || !isValidPin(pEn) || !isValidPin(pSda) || !isValidPin(pScl)) err = "Broche invalide.";
    else if (!pinsAreDistinct(pRx, pTx, pEn, pSda, pScl)) err = "Chaque fonction doit utiliser une broche différente.";
    else if (hauteur < 101 || hauteur > 1999) err = "Hauteur de cuve invalide (101 à 1999 mm).";
    else if (webAuth && (wUser.length() < 1 || wUser.length() > 32 || wUser.indexOf(':') >= 0)) err = "Utilisateur web invalide (1 à 32 caractères, sans « : »).";
    else if (webAuth && (wPass.length() < 6 || wPass.length() > 63)) err = "Mot de passe web invalide (6 à 63 caractères).";

    if (err.length() > 0)
    {
        webServer.send(400, "text/plain; charset=utf-8", err);
        return;
    }

    saveConfig(ssid, pass, mHost, (uint16_t)mPort, mUser, mPass, pRx, pTx, pEn, pSda, pScl, wUser, wPass);
    saveHostname(host);
    saveHauteur(hauteur);
    Serial.println("Configuration enregistree, redemarrage");

    webServer.send(200, "text/plain; charset=utf-8",
        "Configuration enregistrée. L'ESP redémarre : reconnectez votre appareil à votre réseau Wi-Fi, puis ouvrez http://" + host +
        ".local (ou l'adresse IP de l'ESP, visible dans votre routeur ou dans Home Assistant). Si la connexion échoue, le point d'accès « " AP_SSID " » réapparaît.");
    delay(500);   // laisse le temps d'envoyer la reponse
    ESP.restart();
}

//POST /config/reset : efface la configuration et redemarre en mode configuration initiale
void handleConfigReset()
{
    if (!checkAuth()) return;
    Serial.println("HTTP config/reset appele");
    clearConfig();
    webServer.send(200, "text/plain; charset=utf-8", "Configuration effacée. Redémarrage en mode configuration : connectez-vous au Wi-Fi « " AP_SSID " ».");
    delay(500);   // laisse le temps d'envoyer la reponse
    ESP.restart();
}

//Declare les routes web. En mode configuration, seules les routes de configuration sont actives.
void setupWebServer()
{
    webServer.on("/", HTTP_GET, handleRoot);
    webServer.on("/config", HTTP_GET, handleConfigPage);
    webServer.on("/api/config", HTTP_GET, handleConfigGet);
    webServer.on("/api/config", HTTP_POST, handleConfigSave);
    webServer.on("/api/scan", HTTP_GET, handleScan);
    webServer.on("/config/reset", HTTP_POST, handleConfigReset);
    webServer.onNotFound(handleNotFound);

    if (!portalMode)
    {
        webServer.on("/api/status", HTTP_GET, handleStatus);
        webServer.on("/set", HTTP_POST, handleSet);
        webServer.on("/reboot", HTTP_POST, handleReboot);
        webServer.on("/align/start", HTTP_POST, handleAlignStart);
        if (webAuthEnabled()) httpUpdater.setup(&webServer, "/update", cfgWebUser.c_str(), cfgWebPass.c_str());
        else httpUpdater.setup(&webServer, "/update");
    }
    webServer.begin();
}

//Connexion au Wi-Fi saisi dans la configuration. Renvoie false si la connexion n'aboutit pas dans le delai.
bool connectWifi()
{
    Serial.println("");
    Serial.println("Wifi : connexion a " + cfgSsid);
    WiFi.persistent(false);   // les identifiants viennent de notre EEPROM : pas d'ecriture inutile dans la flash du SDK
    WiFi.mode(WIFI_STA);
    WiFi.hostname(espHostname);
    WiFi.setAutoReconnect(true);
    WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS)
    {
        delay(250);
    }
    if (WiFi.status() != WL_CONNECTED) return false;

    Serial.println("Wifi : connected");
    Serial.print("Wifi : hostname: ");
    Serial.println(espHostname);
    Serial.print("Wifi : IP address: ");
    Serial.println(WiFi.localIP());
    return true;
}

//Ouvre le point d'acces de configuration et la page /config (portail captif).
//firstSetup = true : aucune configuration stockee. false : configuration presente mais Wi-Fi injoignable.
void startPortal(bool firstSetup)
{
    portalMode = true;
    portalFirstSetup = firstSetup;
    lastWebActivity = millis();

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    scanCache = scanNetworksJson();   // avant d'ouvrir le point d'acces : une recherche en mode AP coupe les connexions

    WiFi.mode(WIFI_AP_STA);
    if (strlen(AP_PASS) >= 8) WiFi.softAP(AP_SSID, AP_PASS);
    else WiFi.softAP(AP_SSID);
    delay(200);

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.start(53, "*", WiFi.softAPIP());   // toute adresse tapee renvoie vers l'ESP

    setupWebServer();

    Serial.print("Portail de configuration : Wi-Fi \"" AP_SSID "\", page http://");
    Serial.print(WiFi.softAPIP());
    Serial.println("/config");
}

void setup()
{
    Serial.begin(115200);
    Serial.println();
    Serial.println("Micronova controller - firmware " FW_VERSION);

    //Configuration : hostname, hauteur de cuve et reglages saisis sur la page web
    loadHostname();   // appelle EEPROM.begin()
    loadHauteur();
    loadConfig();

    if (!cfgConfigured)
    {
        //Premier demarrage : point d'acces "Micronova-Setup" + page de configuration
        Serial.println("Aucune configuration enregistree : demarrage du portail de configuration");
        startPortal(true);
        return;
    }

    //Broches : liaison poele et optocoupleur
    pinMode(pinEnableRx, OUTPUT);
    digitalWrite(pinEnableRx, HIGH); //The led of the optocoupler is off
    StoveSerial.begin(1200, SERIAL_MODE, pinRx, pinTx, false, 256);

    //Init Wifi
    if (!connectWifi())
    {
        Serial.println("Wifi : connexion impossible, ouverture du point d'acces de configuration");
        startPortal(false);
        return;
    }

    setupWebServer();

    // OTA depuis l'IDE Arduino / PlatformIO
    ArduinoOTA.setHostname(espHostname.c_str());
    if (webAuthEnabled()) ArduinoOTA.setPassword(cfgWebPass.c_str());
    ArduinoOTA.onStart([]() { Write_Log(client, "OTA : debut de la mise a jour"); });
    ArduinoOTA.onEnd([]() { Write_Log(client, "OTA : termine, redemarrage"); });
    ArduinoOTA.onError([](ota_error_t error) { Write_Log(client, "OTA : erreur " + String((int)error)); });
    ArduinoOTA.begin();

    Write_Log(client, "WEB : Serveur HTTP démarré");

    //Init MQTT (si le serveur est injoignable, la page web reste disponible pour corriger la configuration)
    reconnect_mqtt();
    client.subscribe(cmd_topic);
    client.subscribe(hauteur_topic); // abonner au topic hauteur


    // Petit délai pour recevoir le dernier message MQTT sur hauteur
    unsigned long start = millis();
    while (millis() - start < 2000) { // attendre 2 secondes max
        client.loop(); // process MQTT messages
    }

    //Write logs

    Write_Log(client,"Setup : Version " FW_VERSION " (" __DATE__ " " __TIME__ ")");
    Write_Log(client,"Setup : Wifi ok");
    Write_Log(client,"Setup : MQTT ok");
    Write_Log(client,"Setup : broches GPIO RX=" + String(pinRx) + " TX=" + String(pinTx) + " ENABLE_RX=" + String(pinEnableRx) + " SDA=" + String(pinSda) + " SCL=" + String(pinScl));


    //Init VL53L0X sensor

    Wire.begin(pinSda, pinScl);
    Wire.setClock(100000);
    sensorInitialized = lox.begin();
    if (!sensorInitialized) {
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

    // Premier heartbeat immediat (sans attendre 60 s)
    updateSensorHealth();
    publishHeartbeat();

}

void loop()
{
    //Mode configuration : seuls le DNS (portail captif) et la page web tournent
    if (portalMode)
    {
        dnsServer.processNextRequest();
        webServer.handleClient();

        //Configuration presente mais Wi-Fi injoignable (box encore en demarrage apres une coupure ?) :
        //nouvel essai de connexion apres 5 min sans activite sur la page
        if (!portalFirstSetup && millis() - lastWebActivity > PORTAL_RETRY_MS)
        {
            Serial.println("Portail : nouvel essai de connexion au Wi-Fi");
            ESP.restart();
        }
        return;
    }
    
    
    if (!client.connected())
    {
        reconnect_mqtt();
		client.subscribe(cmd_topic);
    }
    
        client.loop();
        webServer.handleClient();
        ArduinoOTA.handle();
        alignTick();   // mesures en continu si le mode alignement est actif

    
    unsigned long currentMillis = millis();
    if (previousMillis > currentMillis)
    {
        previousMillis = 0;
    }
    if (currentMillis - previousMillis >= 60000)  // délais entre chaque cycle 60s
    {
        previousMillis = currentMillis;
        getStates();
        updateSensorHealth();
        publishHeartbeat();
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