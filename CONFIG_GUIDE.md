# SDM120 MQTT Monitor - Configuration Guide

## 🔒 Secure Configuration with Kconfig

All sensitive settings (credentials, IP addresses, etc.) are now externalized through ESP-IDF's Kconfig system. This keeps credentials out of your source code and makes the project more secure and flexible.

## 📋 Quick Setup

### 1. Configure Settings
```bash
cd $HOME/esp/sdm120-mqtt
idf.py menuconfig
```

### 2. Navigate to Configuration Menus

#### 🏠 **SDM120 Device Configuration**
- **SDM120 Device IP Address**: `192.168.1.11` (your meter's IP)
- **SDM120 Device Port**: `502` (Modbus TCP port)
- **Modbus Response Timeout**: `5000ms` (increase if getting timeouts)
- **Inter-Parameter Delay**: `200ms` (delay between readings)

#### 📡 **SDM120 MQTT Configuration**
- **MQTT Broker URI**: `mqtt://192.168.1.10:1883` (your broker)
- **MQTT Username**: `esp32user` (your MQTT username)
- **MQTT Password**: `S3cr3t` (your MQTT password)
- **MQTT Client ID**: `sdm120_esp32` (unique identifier)
- **MQTT Topic Prefix**: `energy/sdm120` (base topic)
- **Enable Home Assistant MQTT Discovery**: `Yes` (for HA integration)
- **Home Assistant Discovery Prefix**: `homeassistant` (HA discovery topic)

#### 🌐 **WiFi Configuration**
- **WiFi SSID**: Your WiFi network name (required)
- **WiFi Password**: Your WiFi password (leave empty for open networks)
- **Maximum WiFi Connection Retry**: `5` (retry attempts before giving up)
- **WiFi Connection Timeout**: `10000ms` (connection establishment timeout)
- **WiFi Reconnect Interval**: `5000ms` (time between reconnection attempts)
- **WiFi Power Save Mode**: `No Power Save` (recommended for Modbus reliability)

### 3. Build and Flash
```bash
idf.py build flash monitor
```

## 🔐 Security Features

### ✅ **What's Secured:**
- ✅ MQTT username and password are external
- ✅ WiFi credentials are external
- ✅ Device IP addresses are configurable
- ✅ All settings are in `sdkconfig` (can be `.gitignore`d)
- ✅ No hardcoded credentials in source code

### 📝 **Configuration Files:**
- `sdkconfig` - Generated configuration (contains your settings)
- `sdkconfig.defaults` - Default values (safe to commit)
- `main/Kconfig.projbuild` - Configuration options (safe to commit)

## 🔧 Advanced Configuration

### Change Settings Later
```bash
idf.py menuconfig  # Modify any settings
idf.py build flash monitor  # Apply changes
```

### Reset to Defaults
```bash
rm sdkconfig
idf.py menuconfig  # Will use defaults from sdkconfig.defaults
```

### Environment-Specific Configs
```bash
# Development
cp sdkconfig sdkconfig.dev

# Production  
cp sdkconfig sdkconfig.prod

# Switch between configs
cp sdkconfig.prod sdkconfig
idf.py build flash
```

## 📡 **Native WiFi Features**

The SDM120 monitor now includes a robust native WiFi implementation with:

### ✅ **Robust Connection Management:**
- **Automatic Reconnection**: Continuously monitors WiFi status and reconnects when needed
- **Configurable Retry Logic**: Set maximum retry attempts and timeouts
- **Background Monitoring**: Dedicated task monitors connection health
- **Progressive Delays**: Smart retry intervals to avoid overwhelming the network

### ⚡ **Power Management:**
- **Optimized for Modbus**: Default "No Power Save" mode for reliable communication
- **Configurable**: Choose between performance and power consumption
- **Network-Friendly**: Balanced approach for different use cases

### 🔧 **Easy Configuration:**
- **No Hardcoded Credentials**: All settings through `idf.py menuconfig`
- **Comprehensive Options**: Timeouts, retry counts, power modes
- **Environment Flexibility**: Easy switching between networks

### 🚨 **Error Handling:**
- **Connection Validation**: Checks WiFi credentials before attempting connection
- **Timeout Protection**: Prevents indefinite connection attempts
- **Status Monitoring**: Real-time connection status feedback
- **Automatic Recovery**: Seamless reconnection without manual intervention

## 🏠 Home Assistant Integration

With externalized config and native WiFi, you can easily:

1. **Different Devices**: Change `MQTT_CLIENT_ID` and `MQTT_TOPIC_PREFIX` for multiple meters
2. **Different Brokers**: Switch between test/production MQTT brokers  
3. **Different Networks**: Configure different WiFi networks per environment
4. **Robust Connectivity**: Automatic WiFi reconnection with configurable retry logic
5. **Power Management**: Optimized WiFi power settings for Modbus reliability

## 🚨 Security Best Practices

### ✅ **Do This:**
- Add `sdkconfig` to `.gitignore` 
- Use different passwords for each device
- Use unique MQTT client IDs
- Keep `sdkconfig.defaults` with safe defaults only

### ❌ **Don't Do This:**
- Don't commit `sdkconfig` with real credentials
- Don't hardcode passwords in source code
- Don't reuse MQTT client IDs across devices

## 📖 Configuration Reference

| Setting | Menu | Default                    | Description |
|---------|------|----------------------------|-------------|
| Device IP | SDM120 Device Configuration | `192.168.1.11`             | Your SDM120 meter IP address |
| MQTT Broker | SDM120 MQTT Configuration | `mqtt://192.168.1.10:1883` | Your MQTT broker URL |
| MQTT Username | SDM120 MQTT Configuration | `espuser`                  | MQTT authentication username |
| MQTT Password | SDM120 MQTT Configuration | `S3cr3t`                   | MQTT authentication password |
| WiFi SSID | WiFi Configuration | (empty)                    | Your WiFi network name |
| WiFi Password | WiFi Configuration | (empty)                    | Your WiFi network password |
| WiFi Max Retry | WiFi Configuration | `5`                       | Maximum connection retry attempts |
| WiFi Timeout | WiFi Configuration | `10000ms`                 | Connection establishment timeout |
| WiFi Reconnect Interval | WiFi Configuration | `5000ms`                  | Time between reconnection attempts |

## 🎯 Ready!

Your SDM120 MQTT monitor now has:
- 🔒 Secure externalized credentials
- 📡 Native robust WiFi with auto-reconnection
- 🏠 Home Assistant auto-discovery
- 📊 Complete energy monitoring
- 🔧 Easy configuration management
- ⚡ Optimized power management for reliability

Configure with `idf.py menuconfig` and you're ready to go! 🚀 