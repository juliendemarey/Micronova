# Contrôleur Micronova (ESP8266) — Documentation technique

Document rédigé d'après le code du firmware **version 0.6** (`botero2_Nov25_hostname.ino`).

## Sommaire

1. [Description générale du projet](#1-description-générale-du-projet)
2. [Communication via le port série](#2-communication-via-le-port-série)
3. [Logique de lecture et d'écriture des valeurs](#3-logique-de-lecture-et-décriture-des-valeurs)
4. [Les fonctions et leurs usages](#4-les-fonctions-et-leurs-usages)
5. [Focus : l'autodiscovery MQTT](#5-focus--lautodiscovery-mqtt)
6. [Annexe : points d'attention connus](#6-annexe--points-dattention-connus)

---

## 1. Description générale du projet

### Objectif

Ce firmware transforme un ESP8266 en passerelle entre un **poêle à pellets équipé d'une carte Micronova** et **Home Assistant** (HA). Il permet de :

- lire l'état du poêle (arrêté, allumage, chauffage, alarme…), la température ambiante, la puissance et la consigne ;
- commander le poêle (marche, arrêt, stand-by, puissance, consigne de température) ;
- mesurer le niveau de pellets dans la cuve avec un capteur de distance VL53L0X ;
- déclarer automatiquement tous les capteurs et commandes dans HA (MQTT discovery) ;
- se configurer et se mettre à jour sans câble (page web, OTA).

### Architecture

```
 ┌──────────────┐  série 1200 bauds   ┌─────────────┐   Wi-Fi / MQTT   ┌──────────────┐
 │ Carte        │◄───────────────────►│  ESP8266    │◄────────────────►│ Broker MQTT  │
 │ Micronova    │  (optocoupleur)     │  (ce code)  │                  │ + Home       │
 │ du poêle     │                     │             │   HTTP (80)      │ Assistant    │
 └──────────────┘                     │  VL53L0X ───┤◄────────────────►│ navigateur   │
                                      │  (I²C)      │   OTA            └──────────────┘
                                      └─────────────┘
```

### Matériel et brochage

| Fonction                    | Broche | Constante dans le code |
|-----------------------------|--------|------------------------|
| Liaison poêle, réception    | D3     | `RX_PIN`               |
| Liaison poêle, émission     | D4     | `TX_PIN`               |
| Activation de la réception  | D2     | `ENABLE_RX`            |
| I²C SDA (VL53L0X)           | D6     | `SDA`                  |
| I²C SCL (VL53L0X)           | D1     | `SCL`                  |

### Bibliothèques

`ESP8266WiFi`, `PubSubClient` (MQTT), `SoftwareSerial` (liaison poêle), `Wire` et `Adafruit_VL53L0X` (capteur), `ESP8266WebServer` et `ESP8266HTTPUpdateServer` (web et mise à jour par navigateur), `ArduinoOTA` (mise à jour depuis l'IDE), `EEPROM` (hostname).

### Fichiers

| Fichier              | Rôle                                                              |
|----------------------|-------------------------------------------------------------------|
| `*.ino`              | Le firmware                                                       |
| `secrets.h`          | Identifiants Wi-Fi, MQTT, OTA (non versionné)                     |
| `secrets.example.h`  | Modèle de `secrets.h`                                             |

### Cycle de fonctionnement

1. **Démarrage** (`setup`) : lecture du hostname en EEPROM, connexion Wi-Fi, démarrage du serveur web et de l'OTA, connexion MQTT, publication de l'autodiscovery, abonnement aux commandes, initialisation du capteur VL53L0X.
2. **Boucle** (`loop`), en continu : maintien de la connexion MQTT, traitement des messages entrants (commandes), service des requêtes web et OTA.
3. **Toutes les 60 secondes** : interrogation du poêle et du capteur (`getStates`), publication d'un message de vie (`pong`). Au bout de plus de 10 mesures de distance en échec, l'ESP redémarre.

---

## 2. Communication via le port série

### Paramètres

| Paramètre          | Valeur                                             |
|--------------------|----------------------------------------------------|
| Débit              | 1200 bauds                                         |
| Format             | 8 bits de données, pas de parité, **2 bits de stop** (`SWSERIAL_8N2`) |
| Implémentation     | `SoftwareSerial` (`StoveSerial`), tampon de 256 octets |
| Broches            | RX = D3, TX = D4                                   |

L'ESP8266 n'a qu'un seul port série matériel, réservé au débogage (`Serial`, 115200 bauds). La liaison avec le poêle utilise donc un port série logiciel sur D3/D4.

### La broche `ENABLE_RX` (D2)

Le code indique que cette broche commande la LED d'un optocoupleur : à `HIGH`, la LED est éteinte et la réception est inhibée. Elle est à `HIGH` par défaut (au démarrage et après chaque lecture). Elle passe à `LOW` juste après l'envoi d'une requête de lecture, pour laisser passer la réponse du poêle, puis revient à `HIGH` dans `checkStoveReply()`.

### Le protocole

La liaison est de type **requête / réponse**, le poêle ne parle que lorsqu'on l'interroge.

**Lecture : 2 octets envoyés, 2 octets reçus**

```
ESP → poêle :  [code de lecture] [adresse]
poêle → ESP :  [somme de contrôle] [valeur]
```

- Le code de lecture vaut `0x00` pour la mémoire RAM (`_RAMR`) ou `0x20` pour l'EEPROM (`_EEPROMR`).
- La somme de contrôle de la réponse vaut `code + adresse + valeur` (sur 8 bits).

Exemple, lecture de la température ambiante (RAM `0x01`), réponse pour 19,5 °C (valeur 39 = `0x27`) :

```
ESP → poêle :  00 01
poêle → ESP :  28 27        (0x28 = 0x00 + 0x01 + 0x27)
```

**Écriture : 4 octets envoyés, pas de réponse lue**

```
ESP → poêle :  [code d'écriture] [adresse] [valeur] [somme de contrôle]
```

- Le code d'écriture vaut `0x80` pour la RAM (`_RAMW`) ou `0xA0` pour l'EEPROM (`_EEPROMW`).
- La somme de contrôle vaut `code + adresse + valeur`, tronquée à 8 bits (`(char)(…)` dans le code).

Exemple, régler la consigne à 19 °C. Le poêle stocke la valeur ×2, donc 38 = `0x26` à l'adresse EEPROM `0x6B` :

```
ESP → poêle :  A0 6B 26 31   (0xA0 + 0x6B + 0x26 = 0x131, tronqué à 0x31)
```

Le code ne lit aucun accusé de réception après une écriture. Il attend simplement 2 secondes (ou relit les états) pour constater le résultat.

### Cartographie mémoire utilisée

**RAM (lecture seule, côté ESP)**

| Adresse | Constante        | Contenu                          | Unité / conversion |
|---------|------------------|----------------------------------|--------------------|
| `0x01`  | `ambTempAddr`    | Température ambiante             | valeur ÷ 2 = °C    |
| `0x03`  | `stovePwrAddr`   | Puissance réelle                 | 1 = Eco, 2 à 6 = P1 à P5 |
| `0x09`  | `blowTempAddr`   | Température de soufflage         | brute              |
| `0x21`  | `stoveStateAddr` | État du poêle (en écriture : marche, arrêt, stand-by) | code 0 à 13 |
| `0x37`  | `extractorAddr`  | Vitesse de l'extracteur          | brute              |
| `0x59`  | `blowSpeedAddr`  | Vitesse de soufflage             | brute              |
| `0x5C`  | `fumeTempAddr`   | Température des fumées           | brute              |
| `0x63`  | `visPelletAddr`  | Vitesse de la vis à pellets      | brute              |
| `0x75`  | `fixVacuumAddr`  | Dépression fixe                  | brute              |
| `0x76`  | `readVacuumAddr` | Dépression lue                   | brute              |
| `0x77`  | `multVacuumAddr` | Multiplicateur de dépression     | brute              |
| `0x9A`  | `cardTempAddr`   | Température de la carte          | valeur ÷ 2 = °C    |

**EEPROM du poêle (lecture et écriture)**

| Adresse | Constante     | Contenu                 | Remarque |
|---------|---------------|-------------------------|----------|
| `0x6B`  | `tempSetAddr` | Consigne de température | valeur stockée = °C × 2 |
| `0xD0`  | `pwrSetAddr`  | Consigne de puissance   | 0 à 5    |

**Codes d'état du poêle (RAM `0x21`)**

| Code | État                  | Code | État                  |
|------|-----------------------|------|-----------------------|
| 0    | Arrêt                 | 7    | Chauffage             |
| 1    | Ventilation initiale  | 8    | Nettoyage creuset     |
| 2    | Préchauffage résistance | 9  | Nettoyage final       |
| 3    | Préchargement pellets | 10   | Attente stand-by      |
| 4    | Attente flamme        | 11   | Stand-by              |
| 5    | Attente allumage      | 12   | Alarme pellets        |
| 6    | Allumage              | 13   | Alarme (à investiguer)|

### Trames d'écriture prédéfinies

Les trames sont pré-calculées en constantes pour éviter tout recalcul :

| Constante                | Trame             | Effet                        |
|--------------------------|-------------------|------------------------------|
| `stoveOn`                | `80 21 01 A2`     | Mettre en marche             |
| `stoveStb`               | `80 21 0B AC`     | Mettre en stand-by           |
| `forceOff`               | `80 21 00 A1`     | Éteindre                     |
| `stovePWA`, `stovePW1`…`stovePW5` | `A0 D0 0n …` | Régler la puissance (n = 0 à 5) |

---

## 3. Logique de lecture et d'écriture des valeurs

### 3.1 Lecture

Chaque lecture suit la même séquence, dans les fonctions `getXxx()` :

```
1. StoveSerial.write(code de lecture)      // 0x00 (RAM) ou 0x20 (EEPROM)
2. delay(1)
3. StoveSerial.write(adresse)
4. digitalWrite(ENABLE_RX, LOW)            // ouvre la réception
5. delay(80)                               // laisse le poêle répondre
6. checkStoveReply()                       // lit et interprète les 2 octets
```

**`checkStoveReply()`** :

1. Lit au plus 2 octets dans le tampon série (`stoveRxData[0]` = somme de contrôle, `[1]` = valeur).
2. Remet `ENABLE_RX` à `HIGH`.
3. Si exactement 2 octets ont été reçus, **déduit l'adresse** de la réponse avec `param = somme de contrôle − valeur`. Comme la somme de contrôle vaut `code + adresse + valeur`, `param` vaut `code + adresse` : l'adresse pour une lecture RAM (code `0x00`), ou `0x20 + adresse` pour l'EEPROM.
4. Un `switch (param)` aiguille la valeur vers la bonne variable, applique la conversion (÷ 2 pour les températures) et la **publie sur MQTT** (retenue).

Grâce à cette déduction, la réponse n'a pas besoin d'être associée à la requête qui l'a provoquée : elle porte elle-même son adresse.

| Valeur reçue (`param`)      | Variable    | Topic MQTT publié                              |
|-----------------------------|-------------|------------------------------------------------|
| `0x01`                      | `ambTemp`   | `micronova/ambtemp` (°C)                       |
| `0x03`                      | `stovePower`| `micronova/stovepower` (Eco, P1…P5)            |
| `0x21`                      | `stoveState`| `micronova/stovestate` (texte) et `micronova/onoff` |
| `0x6B + 0x20` (EEPROM)      | `tempSet`   | `micronova/tempset` (°C), `micronova/settemp` (état du select) |
| `0xD0 + 0x20` (EEPROM)      | `pwrSet`    | `micronova/pwrset`                             |
| `0x09`, `0x5C`, `0x9A`, `0x37`, `0x59`, `0x63`, `0x75`, `0x76`, `0x77` | divers | `blowtemp`, `fumetemp`, `cardtemp`, `extractor`, `blowspeed`, `vispellet`, `fixvac`, `readvac`, `multvac` |

**Cycle périodique** : toutes les 60 s, `getStates()` appelle `getStovePower`, `getStoveState`, `getAmbTemp`, `getTempSet`, `getPowerSet` puis `getQte`. Les autres lectures (fumées, soufflage, vis, dépression…) existent dans le code mais ne sont pas appelées automatiquement.

### 3.2 Écriture (commandes)

Les commandes arrivent par MQTT sur le topic **`micronova/cmd`**. La fonction `callback()` lit le premier caractère du message pour choisir l'action :

| Payload     | Action                        | Détail                                                                 |
|-------------|-------------------------------|------------------------------------------------------------------------|
| `ON`        | Mettre en marche              | Envoie `stoveOn` si l'état est 0 (arrêt) ou supérieur à 10             |
| `ST`        | Stand-by                      | Envoie `stoveStb` si l'état est compris entre 1 et 9                   |
| `OF…`       | Éteindre                      | Envoie `forceOff` quel que soit l'état                                 |
| `P0`…`P5`   | Puissance                     | Choisit la trame `stovePWA`, `stovePW1`…`stovePW5`                     |
| `T<n>`      | Consigne de température       | `n` est lu avec `atof`, arrondi, puis écrit tel quel à l'EEPROM `0x6B` |

**Point clé de la consigne** : la valeur envoyée dans `T<n>` est la valeur **telle que le poêle la stocke**, donc la température × 2. Choisir 19 °C envoie `T38`, 18,5 °C envoie `T37`. C'est le `command_template` du select dans HA (`T{{ ((value | float) * 2) | int }}`) qui fait la multiplication. En lecture, le firmware fait l'opération inverse (÷ 2) avant de publier.

Après une écriture, le firmware attend 2 secondes (`ON`, `ST`, `OF`, `P`) ou 10 ms (`T`). Pour `ON`, `ST`, `OF` et `T`, il appelle ensuite `getStates()` pour republier l'état réel du poêle ; pour `P`, l'appel est commenté dans le code, et la nouvelle puissance n'est relue qu'au cycle suivant. Pour `ON`, `ST` et `OF`, il publie aussi tout de suite un état provisoire sur `micronova/onoff` (`Marche`, `Attente`, `Eteint`).

### 3.3 Mesure du niveau de la cuve

`getQte()` lit le VL53L0X, un capteur de distance placé au-dessus des pellets :

```
quantité (%) = (hauteur_cuve − distance) / hauteur_cuve × 100
```

- `hauteur_cuve` vaut 500 mm par défaut et se règle depuis la page web (100 à 2000 mm).
- Une mesure n'est faite que **tous les 11 cycles** (environ 11 minutes), via le compteur `measure_sleep`.
- Une distance supérieure à 1000 mm est comptée comme une erreur (`count_reboot`). Plus de 10 mesures en erreur consécutives déclenchent un redémarrage de l'ESP ; une mesure valide remet le compteur à zéro. Un statut « hors portée » du capteur (statut 4) est seulement journalisé, sans être compté.
- La distance est publiée à chaque mesure sur `micronova/distance`. La quantité n'est publiée sur `micronova/qte` que si elle varie d'au moins 10 points depuis la dernière valeur publiée.

### 3.4 Stockage local (EEPROM de l'ESP)

Seul le hostname est conservé en EEPROM (64 octets réservés) :

| Octet | Contenu                                 |
|-------|-----------------------------------------|
| 0     | Marqueur `0xA5` (un hostname est stocké)|
| 1     | Longueur du hostname (1 à 32)           |
| 2…    | Caractères du hostname                  |

Sans marqueur valide, ou si le contenu n'est pas un hostname correct, c'est la valeur par défaut `micronova` qui est utilisée.

---

## 4. Les fonctions et leurs usages

### Démarrage et boucle

| Fonction       | Usage |
|----------------|-------|
| `setup()`      | Initialise les broches et les ports série, charge le hostname, se connecte au Wi-Fi, démarre le serveur web (routes `/`, `/set`, `/hostname`, `/reboot`, `/update`) et l'OTA, se connecte à MQTT, lit le capteur VL53L0X. |
| `loop()`       | Rétablit la connexion MQTT si besoin, traite MQTT (`client.loop()`), les requêtes web (`webServer.handleClient()`) et l'OTA (`ArduinoOTA.handle()`). Toutes les 60 s : `getStates()`, message de vie, vérification du compteur d'échecs. |

### Réseau et MQTT

| Fonction            | Usage |
|---------------------|-------|
| `setup_wifi()`      | Applique le hostname, se connecte au Wi-Fi (boucle jusqu'à la connexion), affiche l'IP et la publie sur le topic de log. |
| `reconnect_mqtt()`  | Configure le serveur et la fonction de rappel, se connecte au broker (nouvelle tentative toutes les 5 s), agrandit le tampon à 1024 octets, **publie l'autodiscovery**, puis s'abonne aux topics `cmd` et `hauteur`. |
| `callback()`        | Reçoit les messages MQTT et exécute les commandes (voir 3.2). |
| `mqttPublish()`     | Publie un message avec trace sur le port série et indication de réussite. |
| `Write_Log()`       | Publie un message sur `micronova/log` (non retenu) et l'affiche sur le port série. |

### Dialogue avec le poêle

| Fonction                | Usage |
|-------------------------|-------|
| `checkStoveReply()`     | Lit et interprète une réponse du poêle, puis publie la valeur (voir 3.1). |
| `getStoveState()`       | Lit l'état du poêle (RAM `0x21`). |
| `getAmbTemp()`          | Lit la température ambiante (RAM `0x01`). |
| `getStovePower()`       | Lit la puissance réelle (RAM `0x03`). |
| `getTempSet()`          | Lit la consigne de température (EEPROM `0x6B`). |
| `getPowerSet()`         | Lit la consigne de puissance (EEPROM `0xD0`). |
| `getFumeTemp()`, `getBlowTemp()`, `getCardTemp()`, `getExtractor()`, `getBlowSpeed()`, `getVisPellet()`, `getFixVacuum()`, `getReadVacuum()`, `getMultVacuum()` | Lectures complémentaires (non appelées par défaut). |
| `getQte()`              | Mesure la distance avec le VL53L0X et calcule le niveau de la cuve (voir 3.3). |
| `getStates()`           | Regroupe les lectures du cycle périodique. |

### Page web

| Fonction            | Usage |
|---------------------|-------|
| `htmlPage()`        | Construit la page : dernières mesures (hauteur, quantité, consigne, température, état, version, hostname), formulaires de hauteur, de hostname et de redémarrage, lien vers `/update`. |
| `handleRoot()`      | Route `GET /` : envoie la page. |
| `handleSet()`       | Route `POST /set` : enregistre la hauteur de la cuve (valeur strictement comprise entre 100 et 2000). |
| `handleHostname()`  | Route `POST /hostname` : valide, enregistre en EEPROM et redémarre pour appliquer le nouveau nom. |
| `handleReboot()`    | Route `POST /reboot` : redémarre l'ESP. |
| `httpUpdater`       | Route `/update` : mise à jour du firmware depuis le navigateur, protégée par identifiant et mot de passe. |

<img width="570" height="900" alt="image" src="https://github.com/user-attachments/assets/9a9f33a1-c6d5-4b87-948b-292a15e6c8a0" />


### Hostname

| Fonction              | Usage |
|-----------------------|-------|
| `isValidHostname()`   | Vérifie 1 à 32 caractères : lettres, chiffres et tirets, sans tiret au début ni à la fin. |
| `loadHostname()`      | Lit le hostname en EEPROM, ou prend la valeur par défaut. |
| `saveHostname()`      | Écrit le hostname en EEPROM. |

---

## 5. Focus : l'autodiscovery MQTT

### Principe

Plutôt que de déclarer chaque entité à la main dans HA, le firmware **publie lui-même la description** de ses capteurs et commandes sur des topics spéciaux. HA les surveille (préfixe `homeassistant/`) et crée automatiquement les entités correspondantes, regroupées dans un seul appareil.

### Structure d'un topic de découverte

```
homeassistant/<composant>/<node_id>/<object_id>/config
```

Exemples du code :

```
homeassistant/switch/Micronova/Controller/config
homeassistant/sensor/Micronova/Temperature/config
homeassistant/select/micronova/Controller/config
```

Le message est un **JSON**, publié avec l'option **retenue** (`retain = true`) : le broker le garde et le renvoie à HA à chacun de ses démarrages, même si l'ESP est éteint.

### Quand et comment c'est publié

La publication se fait dans `reconnect_mqtt()`, **à chaque connexion au broker** :

1. Connexion au broker (identifiant client `ESPClient`).
2. **`client.setBufferSize(1024)`** : indispensable, car la taille par défaut de PubSubClient (256 octets) est inférieure à celle des messages de découverte, qui seraient sinon perdus sans erreur visible.
3. Pour chaque entité : `client.publish(topic, json, true)`, puis `client.loop()` et `delay(150)` pour ne pas saturer le broker ni l'ESP.
4. Abonnement au topic de commande.

Republier à chaque connexion est sans danger : les messages sont identiques aux précédents et HA ne recrée rien.

### Le bloc « appareil »

Toutes les entités embarquent la même description d'appareil (macro `device_information`), ce qui les regroupe dans HA :

```json
{
  "manufacturer": "Philibert Cheminot",
  "identifiers": ["42396f39-80d2-493b-8e8e-31a70e700bc6"],
  "model": "Micronova Controller",
  "name": "Micronova Controller",
  "sw_version": "0.6"
}
```

`identifiers` est la clé qui rassemble les entités sous un seul appareil. `sw_version` reprend la constante `FW_VERSION`.

### Les entités déclarées

| Composant | Nom           | Topic d'état            | Topic de commande | Remarques |
|-----------|---------------|-------------------------|-------------------|-----------|
| `select`  | Temperature   | `micronova/settemp`     | `micronova/cmd`   | Options 17 à 21 par pas de 0,5. `command_template` : `T{{ ((value \| float) * 2) \| int }}` |
| `switch`  | Controller    | `micronova/onoff`       | `micronova/cmd`   | `payload_on` = `ON`, `payload_off` = `OFF` |
| `sensor`  | Controller    | `micronova/pong`        | —                 | Message de vie (« Connecté ») |
| `sensor`  | Temperature   | `micronova/ambtemp`     | —                 | °C, `device_class: temperature` |
| `sensor`  | Consigne      | `micronova/tempset`     | —                 | °C, `device_class: temperature` |
| `sensor`  | State         | `micronova/stovestate`  | —                 | Texte de l'état du poêle |
| `sensor`  | Distance      | `micronova/distance`    | —                 | mm, `device_class: distance` |
| `sensor`  | Quantite      | `micronova/qte`         | —                 | % |
| `sensor`  | log           | `micronova/log`         | —                 | Messages de journal |
| `sensor`  | hauteur       | `micronova/hauteur`     | —                 | Hauteur de la cuve |

### Anatomie d'un message (exemple : capteur de température)

```json
{
  "name": "Temperature",
  "state_topic": "micronova/ambtemp",
  "qos": 0,
  "device_class": "temperature",
  "state_class": "measurement",
  "unit_of_measurement": "°C",
  "icon": "mdi:thermometer",
  "unique_id": "9db3245e-6ace-4d14-ac07-844cd68d245d",
  "device": { "...": "voir le bloc appareil" }
}
```

- `state_topic` : le topic sur lequel HA lit la valeur.
- `device_class` et `state_class` : permettent à HA de gérer l'unité, l'affichage et les statistiques longue durée.
- **`unique_id`** : identifiant stable de l'entité. HA s'en sert pour retrouver son `entity_id`, son historique et ses automatisations. **Il ne doit jamais changer.**

### Cas particulier : le select de consigne

Le select travaille en °C, alors que le poêle stocke la consigne ×2 :

- **Commande** : le `command_template` multiplie la valeur par 2 avant l'envoi (`19` devient `T38`).
- **État** : le firmware relit la consigne dans l'EEPROM du poêle, puis publie sur `micronova/settemp` un texte qui correspond **exactement** à une option (`19` ou `19.5`). Sans cette correspondance, le select afficherait « inconnu ».

Le code contient aussi, en commentaire, une variante avec une entité `number` (curseur de 15 à 25 par pas de 0,5). Un `number` est une alternative possible au select si l'on veut piloter la consigne depuis une carte thermostat.

### Modifier ou supprimer une entité

| Situation | Que faire |
|-----------|-----------|
| Changer une option, un nom, une icône | Modifier le JSON : au prochain démarrage, HA met l'entité à jour (recharger l'intégration MQTT si besoin). |
| Changer le `unique_id` ou le topic de découverte | HA verra **une nouvelle entité** et gardera l'ancienne en « indisponible » : à éviter. |
| Supprimer une entité | Publier un **message vide, retenu**, sur son topic `…/config` (par exemple avec MQTT Explorer), puis retirer l'appel `publish` du code. |

---

## 6. Annexe : points d'attention connus

Ces points ressortent de la lecture du code. Ils ne sont pas corrigés dans la version 0.6.

1. **Hauteur de cuve non conservée.** `hauteur_cuve` repart à 500 mm à chaque redémarrage. La commande MQTT `H` censée la restaurer lit la valeur sans l'affecter, et n'accepte que les messages de 3 caractères.
2. **Plantage possible sur `P`.** Un message `P` suivi d'un caractère autre que 0 à 5 laisse un pointeur nul (`stovePW`) qui est ensuite utilisé pour l'écriture.
3. **Nom du capteur « hauteur ».** Il est déclaré avec le nom `log`, comme le capteur de journal : sans doute un copier-coller.
4. **États du switch.** Le switch attend `ON` / `OFF` sur `micronova/onoff` (`state_on` / `state_off`), alors que le firmware publie `Marche`, `Eteint`, `Arrêt` ou `Attente`. À vérifier : HA peut ignorer ces messages et ne pas refléter l'état réel.
5. **Attentes bloquantes.** Chaque lecture bloque 80 ms, chaque commande 2 secondes, la mesure du capteur 200 ms de plus, et la reconnexion MQTT jusqu'à 5 secondes par tentative. Pendant ces pauses, la page web, MQTT et l'OTA ne répondent pas.
6. **Lectures inutilisées.** Les fonctions de lecture des fumées, du soufflage, de la vis et des dépressions ne sont pas appelées dans le cycle périodique.
