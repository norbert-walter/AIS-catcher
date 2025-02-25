/*
    Copyright(c) 2021-2025 jvde.github@gmail.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "ADSB.h"

// References:
// https://github.com/antirez/dump1090/blob/master/dump1090.c
// https://github.com/wiedehopf/readsb/blob/dev/mode_s.c
// https://github.com/cjkreklow/go-adsb
// https://pure.tudelft.nl/ws/portalfiles/portal/104877928/11_Book_Manuscript_69_1_10_20210510.pdf

// below is a simplified version of the code from dump1090.c by Antirez
namespace Plane
{

    uint32_t crc_table[112] = {
        0x3935ea, 0x1c9af5, 0xf1b77e, 0x78dbbf, 0xc397db, 0x9e31e9, 0xb0e2f0, 0x587178,
        0x2c38bc, 0x161c5e, 0x0b0e2f, 0xfa7d13, 0x82c48d, 0xbe9842, 0x5f4c21, 0xd05c14,
        0x682e0a, 0x341705, 0xe5f186, 0x72f8c3, 0xc68665, 0x9cb936, 0x4e5c9b, 0xd8d449,
        0x939020, 0x49c810, 0x24e408, 0x127204, 0x093902, 0x049c81, 0xfdb444, 0x7eda22,
        0x3f6d11, 0xe04c8c, 0x702646, 0x381323, 0xe3f395, 0x8e03ce, 0x4701e7, 0xdc7af7,
        0x91c77f, 0xb719bb, 0xa476d9, 0xadc168, 0x56e0b4, 0x2b705a, 0x15b82d, 0xf52612,
        0x7a9309, 0xc2b380, 0x6159c0, 0x30ace0, 0x185670, 0x0c2b38, 0x06159c, 0x030ace,
        0x018567, 0xff38b7, 0x80665f, 0xbfc92b, 0xa01e91, 0xaff54c, 0x57faa6, 0x2bfd53,
        0xea04ad, 0x8af852, 0x457c29, 0xdd4410, 0x6ea208, 0x375104, 0x1ba882, 0x0dd441,
        0xf91024, 0x7c8812, 0x3e4409, 0xe0d800, 0x706c00, 0x383600, 0x1c1b00, 0x0e0d80,
        0x0706c0, 0x038360, 0x01c1b0, 0x00e0d8, 0x00706c, 0x003836, 0x001c1b, 0xfff409,
        0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
        0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000,
        0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000};

    void ADSB::Callsign()
    {
        static const char *cs_table = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
        int len = 0;

        for (int i = 0; i < 8; i++)
        {
            uint32_t c = getBits(40 + (i * 6), 6);
            if (cs_table[c] != '#')
            {
                callsign[len++] = cs_table[c];
            }
        }
        callsign[len] = 0;
    }

    // Decode 13 bit AC altitude field (in DF 20 and others)
    int ADSB::decodeAC13Field()
    {
        int m_bit = msg[3] & (1 << 6);

        if (!m_bit)
        {
            int q_bit = msg[3] & (1 << 4);

            if (q_bit)
            {
                // N is the 11 bit integer resulting from removal of M and Q bits
                int n = ((msg[2] & 31) << 6) | ((msg[3] & 0x80) >> 2) | ((msg[3] & 0x20) >> 1) | (msg[3] & 15);
                return n * 25 - 1000;
            }
        }
        /*
        altitude in meters not implemented
        */
        return ALTITUDE_UNDEFINED;
    }

    // Decode 12 bit AC altitude field (in DF 17 and others)
    int ADSB::decodeAC12Field()
    {
        int q_bit = msg[5] & 1;

        if (q_bit)
        {
            // N is the 11 bit integer after removing Q bit
            int n = ((msg[5] >> 1) << 4) | ((msg[6] & 0xF0) >> 4);
            return n * 25 - 1000;
        }
        return ALTITUDE_UNDEFINED;
    }

    double ADSB::decodeMovement()
    {
        int val = getBits(37, 7);

        if (val == 1)
            return 0.0; // Stopped (v < 0.125 kt)
        if (val >= 2 && val <= 8)
            return 0.125 * (val - 1); // 0.125 kt steps
        if (val >= 9 && val <= 12)
            return 1.0 + 0.25 * (val - 8); // 0.25 kt steps
        if (val >= 13 && val <= 38)
            return 2.0 + 0.5 * (val - 12); // 0.5 kt steps
        if (val >= 39 && val <= 93)
            return 15.0 + 1.0 * (val - 38); // 1 kt steps
        if (val >= 94 && val <= 108)
            return 70.0 + 2.0 * (val - 93); // 2 kt steps
        if (val >= 109 && val <= 123)
            return 100.0 + 5.0 * (val - 108); // 5 kt steps
        if (val == 124)
            return 175.0; // v ≥ 175 kt

        return SPEED_UNDEFINED;
    }

    void ADSB::Decode()
    {
        if (msgtype == '1')
            return;

        df = getBits(0, 5);

        if (!validateLength())
        {
            status = STATUS_ERROR;
            return;
        }

        setCRC();

        switch (df)
        {
        case 0:  // Short Air-Air Surveillance
        case 4:  // Surveillance, Altitude Reply
        case 20: // Comm-B, Altitude Reply
        {
            setCRCandICAO();
            altitude = decodeAC13Field();
        }
        break;

        case 5:  // Surveillance, Identity Reply
        case 21: // Comm-B, Identity Reply, ground or air
        {
            setCRCandICAO();

            int a = ((msg[3] & 0x80) >> 5) | ((msg[2] & 0x02) >> 0) | ((msg[2] & 0x08) >> 3);
            int b = ((msg[3] & 0x02) << 1) | ((msg[3] & 0x08) >> 2) | ((msg[3] & 0x20) >> 5);
            int c = ((msg[2] & 0x01) << 2) | ((msg[2] & 0x04) >> 1) | ((msg[2] & 0x10) >> 4);
            int d = ((msg[3] & 0x01) << 2) | ((msg[3] & 0x04) >> 1) | ((msg[3] & 0x10) >> 4);

            squawk = a * 1000 + b * 100 + c * 10 + d;
            break;
        }
        case 11: // All-Call Reply, ground or air
        {
            hexident = getBits(8, 24);
            hexident_status = HEXINDENT_DIRECT;

            int capability = getBits(5, 3);

            if (capability == 4)
                airborne = 0;
            else if (capability == 5)
                airborne = 1;

            if (!verifyCRC())
            {
                status = STATUS_ERROR;
                return;
            }

            break;
        }
        case 17: // Extended Squitter
        case 18: // Extended Squitter/Supplementary
            hexident = getBits(8, 24);
            hexident_status = HEXINDENT_DIRECT;

            if (!verifyCRC())
            {
                status = STATUS_ERROR;
                return;
            }

            int TC = getBits(32, 5);
            int ST = getBits(37, 3); // ME message subtype

            switch (TC)
            {
            case 1: // Aircraft Identification
            case 2:
            case 3:
            case 4:
                Callsign();
                break;

            case 19: // Airborne Velocity
                if (ST >= 1 && ST <= 4)
                {
                    if (ST == 1 || ST == 2)
                    {
                        int Vew = getBits(46, 10);
                        int Vns = getBits(57, 10);

                        if (Vew && Vns)
                        {
                            bool Dew = getBits(45, 1);
                            bool Dns = getBits(56, 1);

                            Vew = Dew ? -(Vew - 1) : (Vew - 1);
                            Vns = Dns ? -(Vns - 1) : (Vns - 1);

                            speed = sqrt(Vns * Vns + Vew * Vew);
                            heading = atan2(Vew, Vns) * 360.0 / (2 * PI);
                            if (heading < 0)
                                heading += 360;

                            if (ST == 2)
                                speed *= 4;
                        }

                        int VR = getBits(69, 9);
                        if (VR)
                        {
                            bool Svr = getBits(68, 1);
                            vertrate = (VR - 1) * 64 * (Svr ? -1 : 1);
                        }
                        airborne = 1;
                    }
                    // ignore ST 3/4, unknown aircraft speed
                }

                break;

            case 5: // Surface Position
            case 6:
            case 7:
            case 8:
            {
                airborne = 0;

                if (getBits(44, 1))
                    heading = getBits(45, 7) * 360 / 128.0;

                decodeMovement();

                CPR &cpr = getBits(53, 1) ? odd : even;

                cpr.lat = getBits(54, 17);
                cpr.lon = getBits(71, 17);
                cpr.timestamp = rxtime;
                cpr.airborne = false;
            }
            break;

            case 9: // Airborne Position
            case 10:
            case 11:
            case 12:
            case 13:
            case 14:
            case 15:
            case 16:
            case 17:
            case 18:
            {
                altitude = decodeAC12Field();
                airborne = 1;

                CPR &cpr = getBits(53, 1) ? odd : even;

                cpr.lat = getBits(54, 17);
                cpr.lon = getBits(71, 17);
                cpr.timestamp = rxtime;
                cpr.airborne = true;
            }
            break;
            }
            break;
        }
    }

    int ADSB::MOD(int a, int b)
    {
        int res = a % b;
        if (res < 0)
            res += b;
        return res;
    }

    int ADSB::NL(double lat)
    {
        lat = std::abs(lat);

        if (lat == 0)
            return 59;
        else if (lat == 87)
            return 2;
        else if (lat > 87)
            return 1;

        double tmp = 1 - (1 - cos(PI / (2.0 * 15.0))) / pow(cos(PI / 180.0 * abs(lat)), 2);
        return std::floor(2 * PI / acos(tmp));
    }

    bool ADSB::decodeCPR(FLOAT32 lat, FLOAT32 lon, bool use_even)
    {
        if (!even.Valid() || !odd.Valid() || (even.airborne != odd.airborne))
            return false;

        if (even.airborne)
        {
            return decodeCPR_airborne(use_even);
        }
        return decodeCPR_surface(lat, lon, use_even);
    }

    bool ADSB::decodeCPR_airborne(bool use_even)
    {
        constexpr double CPR_SCALE = (double)(1 << 17);

        int j = (int)std::floor(((59.0 * even.lat) - (60.0 * odd.lat)) / CPR_SCALE + 0.5);

        double lat_even = 360.0 / 60 * (MOD(j, 60) + even.lat / CPR_SCALE);
        double lat_odd = 360.0 / 59 * (MOD(j, 59) + odd.lat / CPR_SCALE);

        if (lat_even >= 270.0)
            lat_even -= 360.0;
        if (lat_odd >= 270.0)
            lat_odd -= 360.0;

        int nl = NL(lat_even);

        if (nl != NL(lat_odd))
            return false;

        lat = use_even ? lat_even : lat_odd;

        int ni = MAX(nl - (use_even ? 0 : 1), 1);
        int m = std::floor((even.lon * (nl - 1) - odd.lon * nl) / CPR_SCALE + 0.5);

        double lon_final = use_even ? even.lon : odd.lon;

        lon = (360.0 / ni) * (MOD(m, ni) + lon_final / CPR_SCALE);

        if (lon > 180.0)
            lon -= 360.0;

        latlon_timestamp = use_even ? even.timestamp : odd.timestamp;

        return true;
    }

    bool ADSB::decodeCPR_airborne_reference(bool use_even, FLOAT32 ref_lat, FLOAT32 ref_lon)
    {
        constexpr double CPR_SCALE = (double)(1 << 17);

        CPR &cpr = use_even ? even : odd;
        double d_lat = use_even ? 360.0 / 60 : 360.0 / 59;
        int j = std::floor(ref_lat / d_lat) + std::floor(0.5 + MOD(ref_lat, d_lat) / d_lat - cpr.lat / CPR_SCALE + 0.5);

        lat = d_lat * (j + cpr.lat / CPR_SCALE);

        int ni = NL(lat) - (use_even ? 0 : 1);
        double d_lon = ni > 0 ? 360 / ni : 360;

        int m = std::floor(ref_lon / d_lon) + std::floor(0.5 + (MOD(ref_lon, d_lon) / d_lon) - cpr.lon / CPR_SCALE);

        lon = d_lon * (m + cpr.lon / CPR_SCALE);
        latlon_timestamp = cpr.timestamp;

        return true;
    }

    // based on the example from the 1090 riddle
    bool ADSB::decodeCPR_surface(FLOAT32 ref_lat, FLOAT32 ref_lon, bool use_even)
    {
        static bool warning_given = false;

        if (ref_lat == LAT_UNDEFINED || ref_lon == LON_UNDEFINED)
        {
            if (!warning_given)
            {
                Error() << "ADSB: Reference position is not available. Cannot determine location of onground planes. Provide receiver location with -Z lat lon." << std::endl;
                warning_given = true;
            }
            return false;
        }

        constexpr double CPR_SCALE = (double)(1 << 17);

        int j = (int)std::floor((59 * even.lat - 60 * odd.lat) / CPR_SCALE + 0.5);

        double lat_even = 90.0 / 60 * (MOD(j, 60) + even.lat / CPR_SCALE);
        double lat_odd = 90.0 / 59 * (MOD(j, 59) + odd.lat / CPR_SCALE);

        int nl = NL(lat_even);

        if (nl != NL(lat_odd))
            return false;

        lat = use_even ? lat_even : lat_odd;
        lat -= 90.0 * std::floor((lat - ref_lat + 45.0) / 90.0);

        int ni = MAX(nl - (use_even ? 0 : 1), 1);

        int m = std::floor((even.lon * (nl - 1) - odd.lon * nl) / CPR_SCALE + 0.5);
        double lon_final = use_even ? even.lon : odd.lon;

        lon = (90.0 / ni) * (MOD(m, ni) + lon_final / CPR_SCALE);
        lon -= 90.0 * std::floor((lon - ref_lon + 45.0) / 90.0);

        latlon_timestamp = use_even ? even.timestamp : odd.timestamp;

        return true;
    }

    bool ADSB::decodeCPR_surface_reference(bool use_even, FLOAT32 ref_lat, FLOAT32 ref_lon)
    {
        constexpr double CPR_SCALE = (double)(1 << 17);

        CPR &cpr = use_even ? even : odd;
        double d_lat = use_even ? 90.0 / 60 : 90.0 / 59;
        int j = std::floor(ref_lat / d_lat) + std::floor(0.5 + MOD(ref_lat, d_lat) / d_lat - cpr.lat / CPR_SCALE + 0.5);

        lat = d_lat * (j + cpr.lat / CPR_SCALE);

        int ni = NL(lat) - (use_even ? 0 : 1);
        double d_lon = ni > 0 ? 90 / ni : 90;

        int m = std::floor(ref_lon / d_lon) + std::floor(0.5 + (MOD(ref_lon, d_lon) / d_lon) - cpr.lon / CPR_SCALE);

        lon = d_lon * (m + cpr.lon / CPR_SCALE);
        latlon_timestamp = cpr.timestamp;

        return true;
    }

    void ADSB::Print() const
    {
        std::cout << "RX: " << std::put_time(std::localtime(&rxtime), "%F %T") << std::endl;

        std::cout << "MSG: ";
        for (int i = 0; i < len; i++)
        {
            std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)msg[i];
        }
        std::cout << std::dec << std::endl;

        if (crc != CRC_UNDEFINED)
            std::cout << "CRC: " << std::hex << crc << std::dec << std::endl;

        if (msgtype != MSG_TYPE_UNDEFINED)
            std::cout << "MSGTYPE: " << msgtype << std::endl;

        if (df != DF_UNDEFINED)
            std::cout << "DF: " << df << std::endl;

        if (hexident != HEXIDENT_UNDEFINED)
            std::cout << "HEX: " << std::hex << hexident << std::dec << (hexident_status == HEXINDENT_DIRECT ? " (direct)" : " (crc)") << std::endl;

        if (altitude != ALTITUDE_UNDEFINED)
            std::cout << "ALT: " << altitude << std::endl;

        if (lat != LAT_UNDEFINED)
            std::cout << "LAT: " << lat << std::endl;

        if (lon != LON_UNDEFINED)
            std::cout << "LON: " << lon << std::endl;

        if (speed != SPEED_UNDEFINED)
            std::cout << "SPD: " << speed << std::endl;

        if (heading != HEADING_UNDEFINED)
            std::cout << "HDG: " << heading << std::endl;

        if (vertrate != VERT_RATE_UNDEFINED)
            std::cout << "V/S: " << vertrate << std::endl;

        if (squawk != SQUAWK_UNDEFINED)
            std::cout << "SQK: " << squawk << std::endl;

        if (airborne != AIRBORNE_UNDEFINED)
            std::cout << "AIR: " << airborne << std::endl;

        if (callsign[0] != '\0')
            std::cout << "CSN: " << callsign << std::endl;

        if (odd.lat != CPR_POSITION_UNDEFINED)
            std::cout << "ODD: " << odd.lat << ", " << odd.lon << std::endl;

        if (even.lat != CPR_POSITION_UNDEFINED)
            std::cout << "EVEN: " << even.lat << ", " << even.lon << std::endl;

        if (timestamp != TIME_UNDEFINED)
            std::cout << "TS: " << (long)(timestamp) << std::endl;

        if (signalLevel != LEVEL_UNDEFINED)
            std::cout << "LVL: " << signalLevel << std::endl;
    }
}