#include "OneWireHub.h"
#include "OneWireItem.h"

#include "platform.h"

OneWireHub::OneWireHub(const uint8_t pin)
{
    if (getCpuFrequencyMhz() != 80)
        Serial.println("HUB: CPU FREQUENCY NOT 80MHz! TIMINGS WILL BE INCORRECT!");

    _error = Error::NO_ERROR;

    device_count    = 0;
    device_selected = nullptr;

#if ONEWIREHUB_OVERDRIVE_ENABLE
    od_mode = false;
#endif

    for (uint8_t i = 0; i < _ONEWIREHUB_DEVICE_LIMIT; ++i) { device_list[i] = nullptr; }

    // prepare pin
    pin_bitMask = PIN_TO_BITMASK(pin);
    pin_baseReg = PIN_TO_BASEREG(pin);
    pinMode(pin,
            INPUT); // first port-access should by done by this FN, does more than DIRECT_MODE_....
    DIRECT_WRITE_LOW(pin_baseReg, pin_bitMask);

    // prepare debug:
    if (USE_GPIO_DEBUG)
    {
        debug_bitMask = PIN_TO_BITMASK(GPIO_DEBUG_PIN);
        debug_baseReg = PIN_TO_BASEREG(GPIO_DEBUG_PIN);
        pinMode(GPIO_DEBUG_PIN, OUTPUT);
        DIRECT_WRITE_LOW(debug_baseReg, debug_bitMask);
    }

    static_assert(VALUE_IPL, "Your architecture has not been calibrated yet, please run "
                             "examples/debug/calibrate_by_bus_timing and report instructions per "
                             "loop (IPL) to https://github.com/orgua/OneWireHub");
    static_assert(ONEWIREHUB_TIME_VALUE_MIN > 2,
                  "YOUR ARCHITECTURE IS TOO SLOW, THIS MAY RESULT IN "
                  "TIMING-PROBLEMS"); // it could work though, never tested
}


// attach a sensor to the hub
uint8_t OneWireHub::attach(OneWireItem &sensor)
{
    if (device_count >= _ONEWIREHUB_DEVICE_LIMIT) return 255; // hub is full

    // demonstrate an 1ms-Low-State on the debug pin (only if bus stays high during this time)
    // done here because this FN is always called before hub is used
    if (USE_GPIO_DEBUG)
    {
        static bool calibrate_loop_timing = true;
        if (calibrate_loop_timing)
        {
            calibrate_loop_timing = false;
            waitLoops1ms();
        }
    }

    // find position of next free storage-position
    uint8_t position = 255;
    for (uint8_t i = 0; i < _ONEWIREHUB_DEVICE_LIMIT; ++i)
    {
        // check for already attached sensors
        if (device_list[i] == &sensor) { return i; }
        // store position of first empty space
        if ((position > _ONEWIREHUB_DEVICE_LIMIT) && (device_list[i] == nullptr)) { position = i; }
    }

    if (position == 255) return 255;

    device_list[position] = &sensor;
    device_count++;
    buildIDTree();
    return position;
}

bool OneWireHub::detach(const OneWireItem &sensor)
{
    // find position of sensor
    uint8_t position = 255;
    for (uint8_t i = 0; i < _ONEWIREHUB_DEVICE_LIMIT; ++i)
    {
        if (device_list[i] == &sensor)
        {
            position = i;
            break;
        }
    }

    if (position != 255) return detach(position);

    return false;
}

bool OneWireHub::detach(const uint8_t device_number)
{
    if (device_list[device_number] == nullptr) return false;
    if (device_count == 0) return false;
    if (device_number >= _ONEWIREHUB_DEVICE_LIMIT) return false;

    device_list[device_number] = nullptr;
    device_count--;
    buildIDTree();

    return true;
}


// just look through each bit of each ID and build a tree, so there are n=DeviceCount decision-points
// trade-off: more online calculation, but @4Devices 16byte storage instead of 3*256 byte
uint8_t OneWireHub::getNrOfFirstBitSet(const mask_t mask) const
{
    mask_t _mask = mask;
    for (uint8_t i = 0; i < _ONEWIREHUB_DEVICE_LIMIT; ++i)
    {
        if ((_mask & 1) != 0) return i;
        _mask >>= 1;
    }
    return 0;
}

// return next not empty element in device-list
uint8_t OneWireHub::getIndexOfNextSensorInList(const uint8_t index_start) const
{
    for (uint8_t i = index_start; i < _ONEWIREHUB_TREE_SIZE; ++i)
    {
        if (device_list[i] != nullptr) return i;
    }
    return 0;
}

// gone through the address, store this result
uint8_t OneWireHub::getNrOfFirstFreeIDTreeElement(void) const
{
    for (uint8_t i = 0; i < _ONEWIREHUB_TREE_SIZE; ++i)
    {
        if (idTree[i].id_position == 255) return i;
    }
    return 0;
}

// initial FN to build the ID-Tree
uint8_t OneWireHub::buildIDTree(void)
{
    mask_t mask_devices = 0;
    mask_t bit_mask     = 0x01;

    // build mask
    for (uint8_t i = 0; i < _ONEWIREHUB_DEVICE_LIMIT; ++i)
    {
        if (device_list[i] != nullptr) mask_devices |= bit_mask;
        bit_mask <<= 1;
    }

    for (uint8_t i = 0; i < _ONEWIREHUB_TREE_SIZE; ++i) { idTree[i].id_position = 255; }

    // begin with root-element
    buildIDTree(0, mask_devices); // goto branch

    return 0;
}

// returns the branch that this iteration has worked on
uint8_t OneWireHub::buildIDTree(uint8_t position_IDBit, const mask_t mask_devices)
{
    if (mask_devices == 0) return (255);

    while (position_IDBit < 64)
    {
        mask_t        mask_pos{0};
        mask_t        mask_neg{0};
        const uint8_t pos_byte{static_cast<uint8_t>(position_IDBit >> 3)};
        const uint8_t mask_bit{static_cast<uint8_t>(1 << (position_IDBit & 7))};
        mask_t        mask_id{1};

        // searchIDTree through all active emulated devices
        for (uint8_t id = 0; id < _ONEWIREHUB_DEVICE_LIMIT; ++id)
        {
            if ((mask_devices & mask_id) != 0)
            {
                // if device is in mask differentiate the bitValue
                if ((device_list[id]->ID[pos_byte] & mask_bit) != 0) mask_pos |= mask_id;
                else mask_neg |= mask_id;
            }
            mask_id <<= 1;
        }

        if ((mask_neg != 0) && (mask_pos != 0))
        {
            // there was found a junction
            const uint8_t active_element = getNrOfFirstFreeIDTreeElement();

            idTree[active_element].id_position     = position_IDBit;
            idTree[active_element].device_selected = getNrOfFirstBitSet(mask_devices);
            position_IDBit++;
            idTree[active_element].got_one  = buildIDTree(position_IDBit, mask_pos);
            idTree[active_element].got_zero = buildIDTree(position_IDBit, mask_neg);
            return active_element;
        }

        position_IDBit++;
    }

    // gone through the address, store this result
    uint8_t active_element = getNrOfFirstFreeIDTreeElement();

    idTree[active_element].id_position     = 128;
    idTree[active_element].device_selected = getNrOfFirstBitSet(mask_devices);
    idTree[active_element].got_one         = 255;
    idTree[active_element].got_zero        = 255;

    return active_element;
}


bool OneWireHub::poll(void)
{
    _error = Error::NO_ERROR;

    while (true)
    {
        // this additional check prevents an infinite loop when calling this FN without sensors attached
        if (device_count == 0) return true;

        // Once reset is done, go to next step
        if (checkReset()) return false;

        // Reset is complete, tell the OneWire-Host we are present
        if (showPresence()) return false;

        // Now that the OneWire-Host should know we are here, we will get a command from the Host
        if (recvAndProcessCmd()) return false;

        // on total success we want to start again, because the next reset could only be ~125 us away
    }
}


bool OneWireHub::checkReset(
        void) // there is a specific high-time needed before a reset may occur -->  >120us
{
    static_assert(
            ONEWIREHUB_TIME_RESET_MIN[0] >
                    (ONEWIREHUB_TIME_SLOT_MAX[0] + ONEWIREHUB_TIME_READ_MAX[0]),
            "Timings are wrong"); // last number should read: max(ONEWIREHUB_TIME_WRITE_ZERO,ONEWIREHUB_TIME_READ_MAX)
    static_assert(
            ONEWIREHUB_TIME_READ_MAX[0] > ONEWIREHUB_TIME_WRITE_ZERO[0],
            "switch ONEWIREHUB_TIME_WRITE_ZERO with ONEWIREHUB_TIME_READ_MAX in checkReset(), "
            "because it is bigger (worst case)");
    static_assert(ONEWIREHUB_TIME_RESET_MAX[0] > ONEWIREHUB_TIME_RESET_MIN[0], "Timings are wrong");
#if ONEWIREHUB_OVERDRIVE_ENABLE
    static_assert(ONEWIREHUB_TIME_RESET_MIN[1] >
                          (ONEWIREHUB_TIME_SLOT_MAX[1] + ONEWIREHUB_TIME_READ_MAX[1]),
                  "Timings are wrong");
    static_assert(
            ONEWIREHUB_TIME_READ_MAX[1] > ONEWIREHUB_TIME_WRITE_ZERO[1],
            "switch ONEWIREHUB_TIME_WRITE_ZERO with ONEWIREHUB_TIME_READ_MAX in checkReset(), "
            "because it is bigger (worst case)");
    static_assert(ONEWIREHUB_TIME_RESET_MAX[0] > ONEWIREHUB_TIME_RESET_MIN[1], "Timings are wrong");
#endif

    DIRECT_MODE_INPUT(pin_baseReg, pin_bitMask);

    // is entered if there are two resets within a given time (timeslot-detection can issue this skip)
    if (_error == Error::RESET_IN_PROGRESS)
    {
        _error = Error::NO_ERROR;
        if (waitLoopsWhilePinIs(ONEWIREHUB_TIME_RESET_MIN[od_mode] -
                                        ONEWIREHUB_TIME_SLOT_MAX[od_mode] -
                                        ONEWIREHUB_TIME_READ_MAX[od_mode],
                                false) ==
            0) // last number should read: max(ONEWIREHUB_TIME_WRITE_ZERO,ONEWIREHUB_TIME_READ_MAX)
        {
#if ONEWIREHUB_OVERDRIVE_ENABLE
            const timeOW_t loops_remaining = waitLoopsWhilePinIs(
                    ONEWIREHUB_TIME_RESET_MAX[0],
                    false); // showPresence() wants to start at high, so wait for it
            if (od_mode && ((ONEWIREHUB_TIME_RESET_MAX[0] - ONEWIREHUB_TIME_RESET_MIN[od_mode]) >
                            loops_remaining))
            {
                od_mode = false; // normal reset detected, so leave OD-Mode
            };
#else
            waitLoopsWhilePinIs(ONEWIREHUB_TIME_RESET_MAX[0],
                                false); // showPresence() wants to start at high, so wait for it
#endif
            return false;
        }
    }

    if (!DIRECT_READ(pin_baseReg, pin_bitMask))
        return true; // just leave if pin is Low, don't bother to wait, TODO: really needed?

    // wait for the bus to become low (host-controlled), since we are polling we don't know for how long it was zero
    if (waitLoopsWhilePinIs(ONEWIREHUB_TIME_RESET_TIMEOUT, true) == 0)
    {
        //_error = Error::WAIT_RESET_TIMEOUT;
        return true;
    }

    const timeOW_t loops_remaining = waitLoopsWhilePinIs(ONEWIREHUB_TIME_RESET_MAX[0], false);

    // wait for bus-release by OneWire-Host
    if (loops_remaining == 0)
    {
        _error = Error::VERY_LONG_RESET;
        return true;
    }

#if ONEWIREHUB_OVERDRIVE_ENABLE
    if (od_mode &&
        ((ONEWIREHUB_TIME_RESET_MAX[0] - ONEWIREHUB_TIME_RESET_MIN[0]) > loops_remaining))
    {
        od_mode = false; // normal reset detected, so leave OD-Mode
    };
#endif

    // If the OneWire-Host pulled low for to short this will trigger an error
    //if (loops_remaining > (ONEWIREHUB_TIME_RESET_MAX[0] - ONEWIREHUB_TIME_RESET_MIN[od_mode])) _error = Error::VERY_SHORT_RESET; // could be activated again, like the error above, errorhandling is mature enough now

    return (loops_remaining > (ONEWIREHUB_TIME_RESET_MAX[0] - ONEWIREHUB_TIME_RESET_MIN[od_mode]));
}


bool OneWireHub::showPresence(void)
{
    static_assert(ONEWIREHUB_TIME_PRESENCE_MAX[0] > ONEWIREHUB_TIME_PRESENCE_MIN[0],
                  "Timings are wrong");
#if ONEWIREHUB_OVERDRIVE_ENABLE
    static_assert(ONEWIREHUB_TIME_PRESENCE_MAX[1] > ONEWIREHUB_TIME_PRESENCE_MIN[1],
                  "Timings are wrong");
#endif

    // OneWire-Host will delay it's "Presence" check (bus-read)  after the reset
    waitLoopsWhilePinIs(ONEWIREHUB_TIME_PRESENCE_TIMEOUT,
                        true); // no pinCheck demanded, but this additional check can cut waitTime

    if (USE_GPIO_DEBUG) DIRECT_WRITE_HIGH(debug_baseReg, debug_bitMask);

    // pull the bus low and hold it some time
    DIRECT_WRITE_LOW(pin_baseReg, pin_bitMask);
    DIRECT_MODE_OUTPUT(pin_baseReg, pin_bitMask); // drive output low

    wait(ONEWIREHUB_TIME_PRESENCE_MIN
                 [od_mode]); // stays till the end, because it drives the bus low itself

    DIRECT_MODE_INPUT(pin_baseReg, pin_bitMask); // allow it to float

    if (USE_GPIO_DEBUG) DIRECT_WRITE_LOW(debug_baseReg, debug_bitMask);

    // When the OneWire-Host or other devices release the bus within a given time everything is fine
    if (waitLoopsWhilePinIs(
                (ONEWIREHUB_TIME_PRESENCE_MAX[od_mode] - ONEWIREHUB_TIME_PRESENCE_MIN[od_mode]),
                false) == 0)
    {
        _error = Error::PRESENCE_LOW_ON_LINE;
        return true;
    }

    return false;
}

// note: this FN calls sendBit() & recvBit() but doesn't handle interrupts -> calling FN must do this
void OneWireHub::searchIDTree(void)
{
    uint8_t position_IDBit = 0;
    uint8_t trigger_pos    = 0;
    uint8_t active_device  = idTree[trigger_pos].device_selected;
    uint8_t trigger_bit    = idTree[trigger_pos].id_position;

    while (position_IDBit < 64)
    {
        // if junction is reached, act different
        if (position_IDBit == trigger_bit)
        {
            if (sendBit(false)) return;
            if (sendBit(false)) return;

            const bool bit_recv = recvBit();
            if (_error != Error::NO_ERROR) return;

            // switch to next junction
            trigger_pos = bit_recv ? idTree[trigger_pos].got_one : idTree[trigger_pos].got_zero;

            active_device = idTree[trigger_pos].device_selected;

            trigger_bit = (trigger_pos == 255) ? uint8_t(255) : idTree[trigger_pos].id_position;
        }
        else
        {
            const uint8_t pos_byte = (position_IDBit >> 3);
            const uint8_t mask_bit = (static_cast<uint8_t>(1) << (position_IDBit & (7)));
            bool          bit_send;

            if ((device_list[active_device]->ID[pos_byte] & mask_bit) != 0)
            {
                bit_send = true;
                if (sendBit(true)) return;
                if (sendBit(false)) return;
            }
            else
            {
                bit_send = false;
                if (sendBit(false)) return;
                if (sendBit(true)) return;
            }

            const bool bit_recv = recvBit();
            if (_error != Error::NO_ERROR) return;

            if (bit_send != bit_recv) return;
        }
        position_IDBit++;
    }

    device_selected = device_list[active_device];
}

bool OneWireHub::recvAndProcessCmd(void)
{

    // If the only peripheral device is not multidrop compatible, pass all data handling to the device
    if (device_count == 1u)
    {

        device_selected = device_list[getIndexOfNextSensorInList()];
        // TODO: this might be expensive for weak uC and OW in Overdrive and only one device emulated
        //  -> look into optimizations, i.e.:
        //  - preselect when only one device present?
        //  - move that at the end of switch in default (less impact for all other hosts)

        if (device_selected->skip_multidrop)
        {
            device_selected->duty(this);
            return false;
            // TODO: Find the root-cause. This fixes the issue but may cause other problems.
            //  -> correct exit would be: return (_error != Error::NO_ERROR);
        }
    }

    uint8_t address[8], cmd;
    bool    flag = false;

    recv(&cmd);

    if (_error == Error::RESET_IN_PROGRESS)
        return false; // stay in poll()-loop and trigger another datastream-detection
    if (_error != Error::NO_ERROR) return true;

    switch (cmd)
    {
        case 0xF0: // Search rom

            device_selected = nullptr;
            noInterrupts();
            searchIDTree();
            interrupts();

            // most ICs allow going for duty() right after search
            if ((_error == Error::NO_ERROR) && (device_selected != nullptr) &&
                device_selected->fast_search_rom)
            {
                device_selected->duty(this);
            }

            return false; // always trigger a re-init after searchIDTree

        case 0x69: // overdrive MATCH ROM

#if ONEWIREHUB_OVERDRIVE_ENABLE
            od_mode = true;
            waitLoopsWhilePinIs(ONEWIREHUB_TIME_READ_MAX[0], false);
#endif

        case 0x55: // MATCH ROM - Choose/Select ROM

            device_selected = nullptr;

            if (recv(address, 8)) { break; }

            for (uint8_t i = 0; i < _ONEWIREHUB_DEVICE_LIMIT; ++i)
            {
                if (device_list[i] == nullptr) continue;

                flag = true;
                for (uint8_t j = 0; j < 8; ++j)
                {
                    if (device_list[i]->ID[j] != address[j])
                    {
                        flag = false;
                        break;
                    }
                }

                if (flag)
                {
                    device_selected = device_list[i];
                    break;
                }
            }

            if (!flag) { return true; }

            if (device_selected != nullptr)
            {
                if (USE_GPIO_DEBUG) DIRECT_WRITE_HIGH(debug_baseReg, debug_bitMask);
                device_selected->duty(this);
            }
            break;

        case 0x3C: // overdrive SKIP ROM

#if ONEWIREHUB_OVERDRIVE_ENABLE
            od_mode = true;
            waitLoopsWhilePinIs(ONEWIREHUB_TIME_READ_MAX[0], false);
#endif
        case 0xCC: // SKIP ROM

            // NOTE: If more than one peripheral device is present on the bus,
            // and a read command is issued following the Skip ROM command,
            // data collision will occur on the bus as multiple peripheral devices transmit simultaneously
            if ((device_selected == nullptr) && (device_count == 1))
            {
                device_selected = device_list[getIndexOfNextSensorInList()];
            }
            if (device_selected != nullptr)
            {
                if (USE_GPIO_DEBUG) DIRECT_WRITE_HIGH(debug_baseReg, debug_bitMask);
                device_selected->duty(this);
            }
            break;

        case 0x0F: // OLD READ ROM

            // only usable when there is ONE peripheral device on the bus --> continue to current readRom

        case 0x33: // READ ROM

            // only usable when there is ONE peripheral device on the bus
            if ((device_selected == nullptr) && (device_count == 1))
            {
                device_selected = device_list[getIndexOfNextSensorInList()];
            }
            if (device_selected != nullptr)
            {
                device_selected->sendID(this);

                // most ICs allow to go to duty() without reset
                if ((_error == Error::NO_ERROR) && device_selected->fast_read_rom)
                {
                    device_selected->duty(this);
                }
            }
            return false;

        case 0xEC: // ALARM SEARCH

            // TODO: Alarm searchIDTree command, respond if flag is set
            // is like searchIDTree-rom, but only peripheral devices with triggered alarm will appear
            break;

        case 0xA5: // RESUME COMMAND

            if (device_selected == nullptr) return true;
            if (USE_GPIO_DEBUG) DIRECT_WRITE_HIGH(debug_baseReg, debug_bitMask);
            device_selected->duty(this);
            break;

        default: // Unknown command

            _error     = Error::INCORRECT_ONEWIRE_CMD;
            _error_cmd = cmd;
    }

    if (_error == Error::RESET_IN_PROGRESS) return false;

    return (_error != Error::NO_ERROR);
}


// info: check for errors after calling and break/return if possible, returns true if error is detected
// NOTE: if called separately you need to handle interrupts, should be disabled during this FN
bool OneWireHub::sendBit(const bool value)
{
    const bool writeZero = !value;

    // Wait for bus to rise HIGH, signaling end of last timeslot
    timeOW_t retries = ONEWIREHUB_TIME_SLOT_MAX[od_mode];
    while ((DIRECT_READ(pin_baseReg, pin_bitMask) == 0) && (--retries != 0))
        ;
    if (retries == 0)
    {
        _error = Error::RESET_IN_PROGRESS;
        return true;
    }

    // Wait for bus to fall LOW, start of new timeslot
    retries = ONEWIREHUB_TIME_MSG_HIGH_TIMEOUT;
    while ((DIRECT_READ(pin_baseReg, pin_bitMask) != 0) && (--retries != 0))
        ;
    if (retries == 0)
    {
        _error = Error::AWAIT_TIMESLOT_TIMEOUT_HIGH;
        return true;
    }

    // first difference to inner-loop of read()
    if (writeZero)
    {
        DIRECT_MODE_OUTPUT(pin_baseReg, pin_bitMask);
        retries = ONEWIREHUB_TIME_WRITE_ZERO[od_mode];
    }
    else { retries = ONEWIREHUB_TIME_READ_MAX[od_mode]; }

    while ((DIRECT_READ(pin_baseReg, pin_bitMask) == 0) && (--retries != 0))
        ; // TODO: we should check for (!retries) because there could be a reset in progress...
    DIRECT_MODE_INPUT(pin_baseReg, pin_bitMask);

    return false;
}


// should be the preferred function for writes, returns true if error occurred
bool OneWireHub::send(const uint8_t address[], const uint8_t data_length)
{
    noInterrupts(); // will be enabled at the end of function
    DIRECT_WRITE_LOW(pin_baseReg, pin_bitMask);
    DIRECT_MODE_INPUT(pin_baseReg, pin_bitMask);
    uint8_t bytes_sent = 0;

    for (; bytes_sent < data_length; ++bytes_sent) // loop for sending bytes
    {
        const uint8_t dataByte = address[bytes_sent];

        for (uint8_t bitMask = 0x01; bitMask != 0; bitMask <<= 1) // loop for sending bits
        {
            if (sendBit(static_cast<bool>(bitMask & dataByte)))
            {
                if ((bitMask == 0x01) && (_error == Error::AWAIT_TIMESLOT_TIMEOUT_HIGH))
                    _error = Error::FIRST_BIT_OF_BYTE_TIMEOUT;
                interrupts();
                return true;
            }
        }
        if (USE_GPIO_DEBUG)
        {
            DIRECT_WRITE_HIGH(debug_baseReg, debug_bitMask);
            DIRECT_WRITE_LOW(debug_baseReg, debug_bitMask);
        }
    }
    interrupts();
    return (bytes_sent != data_length);
}

bool OneWireHub::send(const uint8_t address[], const uint8_t data_length, uint16_t &crc16)
{
    noInterrupts(); // will be enabled at the end of function
    DIRECT_WRITE_LOW(pin_baseReg, pin_bitMask);
    DIRECT_MODE_INPUT(pin_baseReg, pin_bitMask);
    uint8_t bytes_sent = 0;

    for (; bytes_sent < data_length; ++bytes_sent) // loop for sending bytes
    {
        uint8_t dataByte = address[bytes_sent];

        for (uint8_t counter = 0; counter < 8; ++counter) // loop for sending bits
        {
            if (sendBit(static_cast<bool>(0x01 & dataByte)))
            {
                if ((counter == 0) && (_error == Error::AWAIT_TIMESLOT_TIMEOUT_HIGH))
                    _error = Error::FIRST_BIT_OF_BYTE_TIMEOUT;
                interrupts();
                return true;
            }

            const uint8_t mix = ((uint8_t) crc16 ^ dataByte) & static_cast<uint8_t>(0x01);
            crc16 >>= 1;
            if (mix != 0) crc16 ^= static_cast<uint16_t>(0xA001);
            dataByte >>= 1;
        }
        if (USE_GPIO_DEBUG)
        {
            DIRECT_WRITE_HIGH(debug_baseReg, debug_bitMask);
            DIRECT_WRITE_LOW(debug_baseReg, debug_bitMask);
        }
    }
    interrupts();
    return (bytes_sent != data_length);
}

bool OneWireHub::send(const uint8_t dataByte) { return send(&dataByte, 1); }

// NOTE: if called separately you need to handle interrupts, should be disabled during this FN
bool OneWireHub::recvBit(void)
{
    // Wait for bus to rise HIGH, signaling end of last timeslot
    timeOW_t retries = ONEWIREHUB_TIME_SLOT_MAX[od_mode];
    while ((DIRECT_READ(pin_baseReg, pin_bitMask) == 0) && (--retries != 0))
        ;
    if (retries == 0)
    {
        _error = Error::RESET_IN_PROGRESS;
        return true;
    }

    // Wait for bus to fall LOW, start of new timeslot
    retries = ONEWIREHUB_TIME_MSG_HIGH_TIMEOUT;
    while ((DIRECT_READ(pin_baseReg, pin_bitMask) != 0) && (--retries != 0))
        ;
    if (retries == 0)
    {
        _error = Error::AWAIT_TIMESLOT_TIMEOUT_HIGH;
        return true;
    }

    // wait a specific time to do a read (data is valid by then), // first difference to inner-loop of write()
    retries = ONEWIREHUB_TIME_READ_MIN[od_mode];
    while ((DIRECT_READ(pin_baseReg, pin_bitMask) == 0) && (--retries != 0))
        ;

    return (retries > 0);
}


bool OneWireHub::recv(uint8_t address[], const uint8_t data_length)
{
    noInterrupts(); // will be enabled at the end of function
    DIRECT_WRITE_LOW(pin_baseReg, pin_bitMask);
    DIRECT_MODE_INPUT(pin_baseReg, pin_bitMask);

    uint8_t bytes_received = 0;
    for (; bytes_received < data_length; ++bytes_received)
    {
        uint8_t value = 0;

        for (uint8_t bitMask = 0x01; bitMask != 0; bitMask <<= 1)
        {
            if (recvBit()) value |= bitMask;
            if (_error != Error::NO_ERROR)
            {
                if ((bitMask == 0x01) && (_error == Error::AWAIT_TIMESLOT_TIMEOUT_HIGH))
                    _error = Error::FIRST_BIT_OF_BYTE_TIMEOUT;
                interrupts();
                return true;
            }
        }

        address[bytes_received] = value;

        if (USE_GPIO_DEBUG)
        {
            DIRECT_WRITE_HIGH(debug_baseReg, debug_bitMask);
            DIRECT_WRITE_LOW(debug_baseReg, debug_bitMask);
        }
    }

    interrupts();
    return (bytes_received != data_length);
}


// should be the preferred function for reads, returns true if error occurred
bool OneWireHub::recv(uint8_t address[], const uint8_t data_length, uint16_t &crc16)
{
    noInterrupts(); // will be enabled at the end of function
    DIRECT_WRITE_LOW(pin_baseReg, pin_bitMask);
    DIRECT_MODE_INPUT(pin_baseReg, pin_bitMask);

    uint8_t bytes_received = 0;
    for (; bytes_received < data_length; ++bytes_received)
    {
        uint8_t value = 0;
        uint8_t mix   = 0;
        for (uint8_t bitMask = 0x01; bitMask != 0; bitMask <<= 1)
        {
            if (recvBit())
            {
                value |= bitMask;
                mix = 1;
            }
            else mix = 0;

            if (_error != Error::NO_ERROR)
            {
                if ((bitMask == 0x01) && (_error == Error::AWAIT_TIMESLOT_TIMEOUT_HIGH))
                    _error = Error::FIRST_BIT_OF_BYTE_TIMEOUT;
                interrupts();
                return true;
            }

            mix ^= static_cast<uint8_t>(crc16) & static_cast<uint8_t>(0x01);
            crc16 >>= 1;
            if (mix != 0) crc16 ^= static_cast<uint16_t>(0xA001);
        }

        address[bytes_received] = value;
        if (USE_GPIO_DEBUG)
        {
            DIRECT_WRITE_HIGH(debug_baseReg, debug_bitMask);
            DIRECT_WRITE_LOW(debug_baseReg, debug_bitMask);
        }
    }

    interrupts();
    return (bytes_received != data_length);
}


void OneWireHub::wait(const uint16_t timeout_us) const
{
    timeOW_t loops = timeUsToLoops(timeout_us);
    bool     state = false;
    while (loops != 0)
    {
        loops = waitLoopsWhilePinIs(loops, state);
        state = !state;
    }
}

void OneWireHub::wait(const timeOW_t loops_wait) const
{
    timeOW_t loops = loops_wait;
    bool     state = false;
    while (loops != 0)
    {
        loops = waitLoopsWhilePinIs(loops, state);
        state = !state;
    }
}


// returns false if pins stays in the wanted state all the time
timeOW_t OneWireHub::waitLoopsWhilePinIs(volatile timeOW_t retries, const bool pin_value) const
{
    if (retries == 0) return 0;
    while ((DIRECT_READ(pin_baseReg, pin_bitMask) == pin_value) && (--retries != 0))
        ;
    return retries;
}

void OneWireHub::waitLoops1ms(void)
{
    if (USE_GPIO_DEBUG)
    {
        constexpr timeOW_t loops_1ms  = 1000_us;
        timeOW_t           loops_left = 1;
        while (loops_left != 0)
        {
            waitLoopsWhilePinIs(loops_1ms, false);
            DIRECT_MODE_INPUT(pin_baseReg, pin_bitMask);
            DIRECT_WRITE_HIGH(debug_baseReg, debug_bitMask);
            loops_left = waitLoopsWhilePinIs(loops_1ms, true);
            DIRECT_WRITE_LOW(debug_baseReg, debug_bitMask);
        }
    }
}

// this calibration calibrates timing with the longest low-state on the OW-Bus.
// first it measures some resets with the millis()-fn to get real timing.
// after that it measures with a waitLoops()-FN to determine the instructions-per-loop-value for the used architecture
timeOW_t OneWireHub::waitLoopsCalibrate(void)
{
    const timeOW_t wait_loops{
            1000000 *
            microsecondsToClockCycles(
                    1)}; // loops before canceling a pin-change-wait, 1s, TODO: change back to constexpr if possible (ardu due / zero are blocking)
    constexpr uint32_t TIME_RESET_MIN_US = 430;

    timeOW_t time_for_reset = 0;
    timeOW_t repetitions    = 10;

    DIRECT_MODE_INPUT(pin_baseReg, pin_bitMask);

    // repetitions the longest low-states on the bus with millis(), assume it is a OW-reset
    while (repetitions-- != 0)
    {
#if defined(ARDUINO_ARCH_ESP8266)
        ESP.wdtFeed();
#endif
        uint32_t time_needed = 0;

        // try to catch a OW-reset each time
        while (time_needed < TIME_RESET_MIN_US)
        {
            if (waitLoopsWhilePinIs(wait_loops, true) == 0) continue;
            const uint32_t time_start = micros();
            waitLoopsWhilePinIs(TIMEOW_MAX, false);
            const uint32_t time_stop = micros();
            time_needed              = time_stop - time_start;
        }

        if (time_needed > time_for_reset) time_for_reset = time_needed;
    }

    timeOW_t loops_for_reset = 0;
    repetitions              = 0;

    noInterrupts();
    while (repetitions++ < REPETITIONS)
    {
#if defined(ARDUINO_ARCH_ESP8266)
        ESP.wdtFeed();
#endif
        if (waitLoopsWhilePinIs(wait_loops, true) == 0) continue;
        const timeOW_t loops_left   = waitLoopsWhilePinIs(TIMEOW_MAX, false);
        const timeOW_t loops_needed = TIMEOW_MAX - loops_left;
        if (loops_needed > loops_for_reset) loops_for_reset = loops_needed;
    }
    interrupts();

    waitLoops1ms();

    const timeOW_t value_ipl = (time_for_reset * microsecondsToClockCycles(1)) / loops_for_reset;

    return value_ipl;
}


void OneWireHub::waitLoopsDebug(void) const
{
    if (USE_SERIAL_DEBUG)
    {
        Serial.println("DEBUG TIMINGS for the HUB (measured in loops):");
        Serial.println("(be sure to update VALUE_IPL in src/OneWireHub_config.h first!)");
        Serial.print("value : \t");
        Serial.print(VALUE_IPL * VALUE1k / microsecondsToClockCycles(1));
        Serial.println(" nanoseconds per loop");
        Serial.print("reset min : \t");
        Serial.println(ONEWIREHUB_TIME_RESET_MIN[od_mode]);
        Serial.print("reset max : \t");
        Serial.println(ONEWIREHUB_TIME_RESET_MAX[od_mode]);
        Serial.print("reset tout : \t");
        Serial.println(ONEWIREHUB_TIME_RESET_TIMEOUT);
        Serial.print("presence min : \t");
        Serial.println(ONEWIREHUB_TIME_PRESENCE_TIMEOUT);
        Serial.print("presence low : \t");
        Serial.println(ONEWIREHUB_TIME_PRESENCE_MIN[od_mode]);
        Serial.print("presence low max : \t");
        Serial.println(ONEWIREHUB_TIME_PRESENCE_MAX[od_mode]);
        Serial.print("msg hi timeout : \t");
        Serial.println(ONEWIREHUB_TIME_MSG_HIGH_TIMEOUT);
        Serial.print("slot max : \t");
        Serial.println(ONEWIREHUB_TIME_SLOT_MAX[od_mode]);
        Serial.print("read1low : \t");
        Serial.println(ONEWIREHUB_TIME_READ_MAX[od_mode]);
        Serial.print("read std : \t");
        Serial.println(ONEWIREHUB_TIME_READ_MIN[od_mode]);
        Serial.print("write zero : \t");
        Serial.println(ONEWIREHUB_TIME_WRITE_ZERO[od_mode]);
        Serial.flush();
    }
}

void OneWireHub::printError(void) const
{
    if (USE_SERIAL_DEBUG)
    {
        if (_error == Error::NO_ERROR) return;
        Serial.print("Error: ");
        if (_error == Error::READ_TIMESLOT_TIMEOUT) Serial.print("read timeslot timeout");
        else if (_error == Error::WRITE_TIMESLOT_TIMEOUT) Serial.print("write timeslot timeout");
        else if (_error == Error::WAIT_RESET_TIMEOUT) Serial.print("reset wait timeout");
        else if (_error == Error::VERY_LONG_RESET) Serial.print("very long reset");
        else if (_error == Error::VERY_SHORT_RESET) Serial.print("very short reset");
        else if (_error == Error::PRESENCE_LOW_ON_LINE) Serial.print("presence low on line");
        else if (_error == Error::READ_TIMESLOT_TIMEOUT_LOW) Serial.print("read timeout low");
        else if (_error == Error::AWAIT_TIMESLOT_TIMEOUT_HIGH) Serial.print("await timeout high");
        else if (_error == Error::PRESENCE_HIGH_ON_LINE) Serial.print("presence high on line");
        else if (_error == Error::INCORRECT_ONEWIRE_CMD) Serial.print("incorrect onewire command");
        else if (_error == Error::INCORRECT_DEVICE_USAGE)
            Serial.print("peripheral device was used in incorrect way");
        else if (_error == Error::TRIED_INCORRECT_WRITE)
            Serial.print("tried to write in read-slot");
        else if (_error == Error::FIRST_TIMESLOT_TIMEOUT)
            Serial.print("found no timeslot after reset / presence (is OK)");
        else if (_error == Error::FIRST_BIT_OF_BYTE_TIMEOUT)
            Serial.print("first bit of byte timeout");

        if ((_error == Error::INCORRECT_ONEWIRE_CMD) || (_error == Error::INCORRECT_DEVICE_USAGE))
        {
            Serial.print(" [0x");
            Serial.print(_error_cmd, HEX);
            Serial.println("]");
        }
        else { Serial.println(""); }
    }
}

Error OneWireHub::getError(void) const { return (_error); }

bool OneWireHub::hasError(void) const { return (_error != Error::NO_ERROR); }

void OneWireHub::raiseDeviceError(const uint8_t cmd)
{
    _error     = Error::INCORRECT_DEVICE_USAGE;
    _error_cmd = cmd;
}

Error OneWireHub::clearError(void) // and return it if needed
{
    const Error _tmp = _error;
    _error           = Error::NO_ERROR;
    return _tmp;
}
