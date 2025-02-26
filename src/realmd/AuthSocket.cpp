/*
 * Copyright (C) 2005-2011 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2011 MaNGOSZero <https://github.com/mangos/zero>
 * Copyright (C) 2011-2016 Nostalrius <https://nostalrius.org>
 * Copyright (C) 2016-2017 Elysium Project <https://github.com/elysium-project>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/** \file
    \ingroup realmd
*/

#include "Common.h"
#include "Auth/Hmac.h"
#include "Auth/base32.h"
#include "Database/DatabaseEnv.h"
#include "Config/Config.h"
#include "Log.h"
#include "RealmList.h"
#include "AuthSocket.h"
#include "AuthCodes.h"
#include "PatchHandler.h"
#include "Util.h"

#ifdef USE_SENDGRID
#include "MailerService.h"
#include "SendgridMail.h"
#endif

#include <openssl/md5.h>
#include <ctime>
//#include "Util.h" -- for commented utf8ToUpperOnlyLatin

#include <ace/OS_NS_unistd.h>
#include <ace/OS_NS_fcntl.h>
#include <ace/OS_NS_sys_stat.h>

enum AccountFlags
{
    ACCOUNT_FLAG_GM         = 0x00000001,
    ACCOUNT_FLAG_TRIAL      = 0x00000008,
    ACCOUNT_FLAG_PROPASS    = 0x00800000,
};

// GCC have alternative #pragma pack(N) syntax and old gcc version not support pack(push,N), also any gcc version not support it at some paltform
#if defined( __GNUC__ )
#pragma pack(1)
#else
#pragma pack(push,1)
#endif

typedef struct AUTH_LOGON_CHALLENGE_C
{
    uint8   cmd;
    uint8   error;
    uint16  size;
    uint8   gamename[4];
    uint8   version1;
    uint8   version2;
    uint8   version3;
    uint16  build;
    uint8   platform[4];
    uint8   os[4];
    uint8   country[4];
    uint32  timezone_bias;
    uint32  ip;
    uint8   I_len;
    uint8   I[1];
} sAuthLogonChallenge_C;

//typedef sAuthLogonChallenge_C sAuthReconnectChallenge_C;
/*
typedef struct
{
    uint8   cmd;
    uint8   error;
    uint8   unk2;
    uint8   B[32];
    uint8   g_len;
    uint8   g[1];
    uint8   N_len;
    uint8   N[32];
    uint8   s[32];
    uint8   unk3[16];
} sAuthLogonChallenge_S;
*/

struct sAuthLogonProof_C_Base
{
    uint8   cmd;
    uint8   A[32];
    uint8   M1[20];
    uint8   crc_hash[20];
    uint8   number_of_keys;
};

struct sAuthLogonProof_C_1_11 : public sAuthLogonProof_C_Base
{
    uint8   securityFlags;                                  // 0x00-0x04
};
/*
typedef struct
{
    uint16  unk1;
    uint32  unk2;
    uint8   unk3[4];
    uint16  unk4[20];
}  sAuthLogonProofKey_C;
*/
typedef struct AUTH_LOGON_PROOF_S_BUILD_8089
{
    uint8   cmd;
    uint8   error;
    uint8   M2[20];
    uint32  accountFlags;                                   // see enum AccountFlags
    uint32  surveyId;                                       // SurveyId
    uint16  loginFlags;                                     // some flags (AccountMsgAvailable = 0x01)
} sAuthLogonProof_S_BUILD_8089;

typedef struct AUTH_LOGON_PROOF_S_BUILD_6299
{
    uint8   cmd;
    uint8   error;
    uint8   M2[20];
    uint32  surveyId;                                       // SurveyId
    uint16  loginFlags;                                     // some flags (AccountMsgAvailable = 0x01)
} sAuthLogonProof_S_BUILD_6299;

typedef struct AUTH_LOGON_PROOF_S
{
    uint8   cmd;
    uint8   error;
    uint8   M2[20];
    uint32  surveyId;                                       // SurveyId
} sAuthLogonProof_S;

typedef struct AUTH_RECONNECT_PROOF_C
{
    uint8   cmd;
    uint8   R1[16];
    uint8   R2[20];
    uint8   R3[20];
    uint8   number_of_keys;
} sAuthReconnectProof_C;

typedef struct XFER_INIT
{
    uint8 cmd;                                              // XFER_INITIATE
    uint8 fileNameLen;                                      // strlen(fileName);
    uint8 fileName[5];                                      // fileName[fileNameLen]
    uint64 file_size;                                       // file size (bytes)
    uint8 md5[MD5_DIGEST_LENGTH];                           // MD5
}XFER_INIT;

typedef struct AuthHandler
{
    eAuthCmd cmd;
    uint32 status;
    bool (AuthSocket::*handler)(void);
}AuthHandler;

// GCC have alternative #pragma pack() syntax and old gcc version not support pack(pop), also any gcc version not support it at some paltform
#if defined( __GNUC__ )
#pragma pack()
#else
#pragma pack(pop)
#endif

#define AUTH_TOTAL_COMMANDS sizeof(table)/sizeof(AuthHandler)

std::array<uint8, 16> VersionChallenge = { { 0xBA, 0xA3, 0x1E, 0x99, 0xA0, 0x0B, 0x21, 0x57, 0xFC, 0x37, 0x3F, 0xB3, 0x69, 0xCD, 0xD2, 0xF1 } };

/// Constructor - set the N and g values for SRP6
AuthSocket::AuthSocket() : promptPin(false), gridSeed(0), _geoUnlockPIN(0), _accountId(0), _lastRealmListRequest(0)
{
    N.SetHexStr("894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7");
    g.SetDword(7);
    _status = STATUS_CHALLENGE;

    _accountDefaultSecurityLevel = SEC_PLAYER;

    _build = 0;
    patch_ = ACE_INVALID_HANDLE;
}

/// Close patch file descriptor before leaving
AuthSocket::~AuthSocket()
{
    if(patch_ != ACE_INVALID_HANDLE)
        ACE_OS::close(patch_);
}

AccountTypes AuthSocket::GetSecurityOn(uint32 realmId) const
{
    AccountSecurityMap::const_iterator it = _accountSecurityOnRealm.find(realmId);
    if (it == _accountSecurityOnRealm.end())
        return _accountDefaultSecurityLevel;
    return it->second;
}

/// Accept the connection and set the s random value for SRP6
void AuthSocket::OnAccept()
{
    BASIC_LOG("Accepting connection from '%s'", get_remote_address().c_str());
}

/// Read the packet from the client
void AuthSocket::OnRead()
{
    // benchmarking has demonstrated that this lookup method is faster than std::map
    const static AuthHandler table[] =
    {
        { CMD_AUTH_LOGON_CHALLENGE,     STATUS_CHALLENGE,   &AuthSocket::_HandleLogonChallenge },
        { CMD_AUTH_LOGON_PROOF,         STATUS_LOGON_PROOF, &AuthSocket::_HandleLogonProof },
        { CMD_AUTH_RECONNECT_CHALLENGE, STATUS_CHALLENGE,   &AuthSocket::_HandleReconnectChallenge },
        { CMD_AUTH_RECONNECT_PROOF,     STATUS_RECON_PROOF, &AuthSocket::_HandleReconnectProof },
        { CMD_REALM_LIST,               STATUS_AUTHED,      &AuthSocket::_HandleRealmList },
        { CMD_XFER_ACCEPT,              STATUS_PATCH,       &AuthSocket::_HandleXferAccept },
        { CMD_XFER_RESUME,              STATUS_PATCH,       &AuthSocket::_HandleXferResume },
        { CMD_XFER_CANCEL,              STATUS_PATCH,       &AuthSocket::_HandleXferCancel }
    };

    uint8 _cmd;
    while (1)
    {
        if(!recv_soft((char *)&_cmd, 1))
            return;

        size_t i;

        ///- Circle through known commands and call the correct command handler
        for (i = 0; i < AUTH_TOTAL_COMMANDS; ++i)
        {
            if (table[i].cmd != _cmd)
                continue;

            // unauthorized
            DEBUG_LOG("[Auth] Status %u, table status %u", _status, table[i].status);

            if (table[i].status != _status)
            {
                DEBUG_LOG("[Auth] Received unauthorized command %u length %u", _cmd, (uint32)recv_len());
                return;
            }

            DEBUG_LOG("[Auth] Got data for cmd %u recv length %u", _cmd, (uint32)recv_len());

            if (!(*this.*table[i].handler)())
            {
                DEBUG_LOG("[Auth] Command handler failed for cmd %u recv length %u", _cmd, (uint32)recv_len());
                close_connection();
                return;
            }

            break;
        }

        ///- Report unknown commands in the debug log
        if (i == AUTH_TOTAL_COMMANDS)
        {
            DEBUG_LOG("[Auth] got unknown packet %u", (uint32)_cmd);
            return;
        }
    }
}

/// Make the SRP6 calculation from hash in dB
void AuthSocket::_SetVSFields(const std::string& rI)
{
    s.SetRand(s_BYTE_SIZE * 8);

    BigNumber I;
    I.SetHexStr(rI.c_str());

    // In case of leading zeros in the rI hash, restore them
    uint8 mDigest[SHA_DIGEST_LENGTH];
    memset(mDigest, 0, SHA_DIGEST_LENGTH);
    if (I.GetNumBytes() <= SHA_DIGEST_LENGTH)
        memcpy(mDigest, I.AsByteArray().data(), I.GetNumBytes());

    std::reverse(mDigest, mDigest + SHA_DIGEST_LENGTH);

    Sha1Hash sha;
    sha.UpdateData(s.AsByteArray());
    sha.UpdateData(mDigest, SHA_DIGEST_LENGTH);
    sha.Finalize();
    BigNumber x;
    x.SetBinary(sha.GetDigest(), sha.GetLength());
    v = g.ModExp(x, N);
    // No SQL injection (username escaped)
    const char *v_hex, *s_hex;
    v_hex = v.AsHexStr();
    s_hex = s.AsHexStr();
    LoginDatabase.PExecute("UPDATE `account` SET `v` = '%s', `s` = '%s' WHERE `username` = '%s'", v_hex, s_hex, _safelogin.c_str() );
    OPENSSL_free((void*)v_hex);
    OPENSSL_free((void*)s_hex);
}

void AuthSocket::SendProof(Sha1Hash sha)
{
    if (_build < 6299)  // before version 2.0.3 (exclusive)
    {
        sAuthLogonProof_S proof;
        memcpy(proof.M2, sha.GetDigest(), 20);
        proof.cmd = CMD_AUTH_LOGON_PROOF;
        proof.error = 0;
        proof.surveyId = 0x00000000;

        send((char *)&proof, sizeof(proof));
    }
    else if (_build < 8089) // before version 2.4.0 (exclusive)
    {
        sAuthLogonProof_S_BUILD_6299 proof;
        memcpy(proof.M2, sha.GetDigest(), 20);
        proof.cmd = CMD_AUTH_LOGON_PROOF;
        proof.error = 0;
        proof.surveyId = 0x00000000;
        proof.loginFlags = 0x0000;

        send((char *)&proof, sizeof(proof));
    }
    else
    {
        sAuthLogonProof_S_BUILD_8089 proof;
        memcpy(proof.M2, sha.GetDigest(), 20);
        proof.cmd = CMD_AUTH_LOGON_PROOF;
        proof.error = 0;
        proof.accountFlags = ACCOUNT_FLAG_PROPASS;
        proof.surveyId = 0x00000000;
        proof.loginFlags = 0x0000;

        send((char *)&proof, sizeof(proof));
    }
}

/// Logon Challenge command handler
bool AuthSocket::_HandleLogonChallenge()
{
    DEBUG_LOG("Entering _HandleLogonChallenge");
    if (recv_len() < sizeof(sAuthLogonChallenge_C))
        return false;

    ///- Read the first 4 bytes (header) to get the length of the remaining of the packet
    std::vector<uint8> buf;
    buf.resize(4);

    recv((char *)&buf[0], 4);

    EndianConvert(*((uint16*)(&buf[0])));
    uint16 remaining = ((sAuthLogonChallenge_C *)&buf[0])->size;
    DEBUG_LOG("[AuthChallenge] got header, body is %#04x bytes", remaining);

    if ((remaining < sizeof(sAuthLogonChallenge_C) - buf.size()) || (recv_len() < remaining))
        return false;

    ///- Session is closed unless overriden
    _status = STATUS_CLOSED;

    //No big fear of memory outage (size is int16, i.e. < 65536)
    buf.resize(remaining + buf.size() + 1);
    buf[buf.size() - 1] = 0;
    sAuthLogonChallenge_C *ch = (sAuthLogonChallenge_C*)&buf[0];

    ///- Read the remaining of the packet
    recv((char *)&buf[4], remaining);
    DEBUG_LOG("[AuthChallenge] got full packet, %#04x bytes", ch->size);
    DEBUG_LOG("[AuthChallenge] name(%d): '%s'", ch->I_len, ch->I);

    // BigEndian code, nop in little endian case
    // size already converted
    EndianConvert(*((uint32*)(&ch->gamename[0])));
    EndianConvert(ch->build);
    EndianConvert(*((uint32*)(&ch->os[0])));
    EndianConvert(*((uint32*)(&ch->country[0])));
    EndianConvert(ch->timezone_bias);
    EndianConvert(ch->ip);

    ByteBuffer pkt;

    _login = (const char*)ch->I;
    _build = ch->build;

    memcpy(&_os, ch->os, sizeof(_os));
    memcpy(&_platform, ch->platform, sizeof(_platform));

    ///- Normalize account name
    //utf8ToUpperOnlyLatin(_login); -- client already send account in expected form

    //Escape the user login to avoid further SQL injection
    //Memory will be freed on AuthSocket object destruction
    _safelogin = _login;
    LoginDatabase.escape_string(_safelogin);

    pkt << (uint8) CMD_AUTH_LOGON_CHALLENGE;
    pkt << (uint8) 0x00;

    ///- Verify that this IP is not in the ip_banned table
    // No SQL injection possible (paste the IP address as passed by the socket)
    std::string address = get_remote_address();
    LoginDatabase.escape_string(address);
    QueryResult *result = LoginDatabase.PQuery("SELECT `unbandate` FROM `ip_banned` WHERE "
    //    permanent                    still banned
        "(`unbandate` = `bandate` OR `unbandate` > UNIX_TIMESTAMP()) AND `ip` = '%s'", address.c_str());
    if (result)
    {
        pkt << (uint8)WOW_FAIL_DB_BUSY;
        BASIC_LOG("[AuthChallenge] Banned ip '%s' tries to login with account '%s'!", get_remote_address().c_str(), _login.c_str());
        delete result;
    }
    else
    {
        ///- Get the account details from the account table
        // No SQL injection (escaped user name)
        result = LoginDatabase.PQuery("SELECT `sha_pass_hash`, `id`, `locked`, `last_ip`, `v`, `s`, `security`, `email_verif`, `geolock_pin`, `email`, UNIX_TIMESTAMP(`joindate`) FROM `account` WHERE `username` = '%s'",_safelogin.c_str ());
        if (result)
        {
            Field* fields = result->Fetch();

            // Prevent login if the user's email address has not been verified
            bool requireVerification = sConfig.GetBoolDefault("ReqEmailVerification", false);
            int32 requireEmailSince = sConfig.GetIntDefault("ReqEmailSince", 0);
            bool verified = (*result)[7].GetBool();
            
            // Prevent login if the user's join date is bigger than the timestamp in configuration
            if (requireEmailSince > 0)
            {
                uint32 t = (*result)[10].GetUInt32();
                requireVerification = requireVerification && (t >= uint32(requireEmailSince));
            }

            if (requireVerification && !verified)
            {
                BASIC_LOG("[AuthChallenge] Account '%s' using IP '%s 'email address requires email verification - rejecting login", _login.c_str(), get_remote_address().c_str());
                pkt << (uint8)WOW_FAIL_UNKNOWN_ACCOUNT;
                send((char const*)pkt.contents(), pkt.size());
                return true;
            }

            ///- If the IP is 'locked', check that the player comes indeed from the correct IP address
            bool locked = false;
            lockFlags = (LockFlag)(*result)[2].GetUInt32();
            securityInfo = (*result)[6].GetCppString();
            _lastIP = fields[3].GetString();
            _geoUnlockPIN = fields[8].GetUInt32();
            _email = fields[9].GetCppString();

            if (lockFlags & IP_LOCK)
            {
                DEBUG_LOG("[AuthChallenge] Account '%s' is locked to IP - '%s'", _login.c_str(), _lastIP.c_str());
                DEBUG_LOG("[AuthChallenge] Player address is '%s'", get_remote_address().c_str());

                if (_lastIP != get_remote_address())
                {
                    DEBUG_LOG("[AuthChallenge] Account IP differs");

                    // account is IP locked and the player does not have 2FA enabled
                    if (((lockFlags & TOTP) != TOTP && (lockFlags & FIXED_PIN) != FIXED_PIN))
                        pkt << (uint8) WOW_FAIL_SUSPENDED;

                    locked = true;
                }
                else
                {
                    DEBUG_LOG("[AuthChallenge] Account IP matches");
                }
            }
            else
            {
                DEBUG_LOG("[AuthChallenge] Account '%s' is not locked to ip", _login.c_str());
            }

            if (!locked || (locked && (lockFlags & FIXED_PIN || lockFlags & TOTP)))
            {
                uint32 account_id = fields[1].GetUInt32();
                ///- If the account is banned, reject the logon attempt
                QueryResult *banresult = LoginDatabase.PQuery("SELECT `bandate`, `unbandate` FROM `account_banned` WHERE "
                    "`id` = %u AND `active` = 1 AND (`unbandate` > UNIX_TIMESTAMP() OR `unbandate` = `bandate`) LIMIT 1", account_id);
                if (banresult)
                {
                    if((*banresult)[0].GetUInt64() == (*banresult)[1].GetUInt64())
                    {
                        pkt << (uint8) WOW_FAIL_BANNED;
                        BASIC_LOG("[AuthChallenge] Banned account '%s' using IP '%s' tries to login!",_login.c_str (), get_remote_address().c_str());
                    }
                    else
                    {
                        pkt << (uint8) WOW_FAIL_SUSPENDED;
                        BASIC_LOG("[AuthChallenge] Temporarily banned account '%s' using IP '%s' tries to login!",_login.c_str (), get_remote_address().c_str());
                    }

                    delete banresult;
                }
                else
                {
                    ///- Get the password from the account table, upper it, and make the SRP6 calculation
                    std::string rI = fields[0].GetCppString();

                    ///- Don't calculate (v, s) if there are already some in the database
                    std::string databaseV = fields[4].GetCppString();
                    std::string databaseS = fields[5].GetCppString();

                    DEBUG_LOG("database authentication values: v='%s' s='%s'", databaseV.c_str(), databaseS.c_str());

                    // multiply with 2, bytes are stored as hexstring
                    if(databaseV.size() != s_BYTE_SIZE*2 || databaseS.size() != s_BYTE_SIZE*2)
                        _SetVSFields(rI);
                    else
                    {
                        s.SetHexStr(databaseS.c_str());
                        v.SetHexStr(databaseV.c_str());
                    }

                    b.SetRand(19 * 8);
                    BigNumber gmod = g.ModExp(b, N);
                    B = ((v * 3) + gmod) % N;

                    MANGOS_ASSERT(gmod.GetNumBytes() <= 32);

                    ///- Fill the response packet with the result
                    pkt << uint8(WOW_SUCCESS);

                    // B may be calculated < 32B so we force minimal length to 32B
                    pkt.append(B.AsByteArray(32));      // 32 bytes
                    pkt << uint8(1);
                    pkt.append(g.AsByteArray());
                    pkt << uint8(32);
                    pkt.append(N.AsByteArray(32));
                    pkt.append(s.AsByteArray());        // 32 bytes
                    pkt.append(VersionChallenge.data(), VersionChallenge.size());

                    // figure out whether we need to display the PIN grid
                    promptPin = locked; // always prompt if the account is IP locked & 2FA is enabled

                    if ((!locked && ((lockFlags & ALWAYS_ENFORCE) == ALWAYS_ENFORCE)) || _geoUnlockPIN)
                    {
                        promptPin = true; // prompt if the lock hasn't been triggered but ALWAYS_ENFORCE is set
                    }

                    if (promptPin)
                    {
                        BASIC_LOG("[AuthChallenge] Account '%s' using IP '%s' requires PIN authentication", _login.c_str(), get_remote_address().c_str());

                        uint32 gridSeedPkt = gridSeed = static_cast<uint32>(rand32());
                        EndianConvert(gridSeedPkt);
                        serverSecuritySalt.SetRand(16 * 8); // 16 bytes random

                        pkt << uint8(1); // securityFlags, only '1' is available in classic (PIN input)
                        pkt << gridSeedPkt;
                        pkt.append(serverSecuritySalt.AsByteArray(16).data(), 16);
                    }
                    else
                    {
                        if (_build >= 5428)        // version 1.11.0 or later
                            pkt << uint8(0);
                    }

                    _localizationName.resize(4);
                    for(int i = 0; i < 4; ++i)
                        _localizationName[i] = ch->country[4-i-1];

                    LoadAccountSecurityLevels(account_id);
                    BASIC_LOG("[AuthChallenge] Account '%s' using IP '%s' is using '%c%c%c%c' locale (%u)", _login.c_str (), get_remote_address().c_str(), ch->country[3], ch->country[2], ch->country[1], ch->country[0], GetLocaleByName(_localizationName));

                    _accountId = account_id;

                    ///- All good, await client's proof
                    _status = STATUS_LOGON_PROOF;
                }
            }
            delete result;
        }
        else                                                // no account
        {
            pkt<< (uint8) WOW_FAIL_UNKNOWN_ACCOUNT;
        }
    }
    send((char const*)pkt.contents(), pkt.size());
    return true;
}

/// Logon Proof command handler
bool AuthSocket::_HandleLogonProof()
{
    DEBUG_LOG("Entering _HandleLogonProof");

    sAuthLogonProof_C_1_11 lp;
    
    ///- Read the packet
    if (_build < 5428)        // before version 1.11.0 (exclusive)
    {
        if (!recv((char *)&lp, sizeof(sAuthLogonProof_C_Base)))
            return false;
        lp.securityFlags = 0;
    }
    else
    {
        if (!recv((char *)&lp, sizeof(sAuthLogonProof_C_1_11)))
            return false;  
    }

    PINData pinData;

    if (lp.securityFlags)
    {
        if (!recv((char*)&pinData, sizeof(pinData)))
            return false;
    }

    ///- Check if the client has one of the expected version numbers
    bool valid_version = FindBuildInfo(_build) != nullptr;

    ///- Session is closed unless overriden
    _status = STATUS_CLOSED;

    /// <ul><li> If the client has no valid version
    if(!valid_version)
    {
        if (this->patch_ != ACE_INVALID_HANDLE)
            return false;

        ///- Check if we have the apropriate patch on the disk
        // file looks like: 65535enGB.mpq
        char tmp[256];

        snprintf(tmp, 256, "%s/%d%s.mpq", sConfig.GetStringDefault("PatchesDir","./patches").c_str(), _build, _localizationName.c_str());

        char filename[PATH_MAX];
        if (ACE_OS::realpath(tmp, filename) != nullptr)
        {
            patch_ = ACE_OS::open(filename, GENERIC_READ | FILE_FLAG_SEQUENTIAL_SCAN);
        }

        if (patch_ == ACE_INVALID_HANDLE)
        {
            // no patch found
            ByteBuffer pkt;
            pkt << (uint8) CMD_AUTH_LOGON_CHALLENGE;
            pkt << (uint8) 0x00;
            pkt << (uint8) WOW_FAIL_VERSION_INVALID;
            DEBUG_LOG("[AuthChallenge] %u is not a valid client version!", _build);
            DEBUG_LOG("[AuthChallenge] Patch %s not found", tmp);
            send((char const*)pkt.contents(), pkt.size());
            return true;
        }

        XFER_INIT xferh;

        ACE_OFF_T file_size = ACE_OS::filesize(this->patch_);

        if (file_size == -1)
        {
            close_connection();
            return false;
        }

        if (!PatchCache::instance()->GetHash(tmp, (uint8*)&xferh.md5))
        {
            // calculate patch md5, happens if patch was added while realmd was running
            PatchCache::instance()->LoadPatchMD5(tmp);
            PatchCache::instance()->GetHash(tmp, (uint8*)&xferh.md5);
        }

        uint8 data[2] = { CMD_AUTH_LOGON_PROOF, WOW_FAIL_VERSION_UPDATE};
        send((const char*)data, sizeof(data));

        memcpy(&xferh, "0\x05Patch", 7);
        xferh.cmd = CMD_XFER_INITIATE;
        xferh.file_size = file_size;

        send((const char*)&xferh, sizeof(xferh));

        // Set right status
        _status = STATUS_PATCH;

        return true;
    }
    /// </ul>

    ///- Continue the SRP6 calculation based on data received from the client
    BigNumber A;

    A.SetBinary(lp.A, 32);

    // SRP safeguard: abort if A==0
    if (A.isZero())
        return false;

    if ((A % N).isZero())
        return false;

    Sha1Hash sha;
    sha.UpdateBigNumbers(&A, &B, nullptr);
    sha.Finalize();
    BigNumber u;
    u.SetBinary(sha.GetDigest(), 20);
    BigNumber S = (A * (v.ModExp(u, N))).ModExp(b, N);

    uint8 t[32];
    uint8 t1[16];
    uint8 vK[40];
    memcpy(t, S.AsByteArray(32).data(), 32);
    for (int i = 0; i < 16; ++i)
    {
        t1[i] = t[i * 2];
    }
    sha.Initialize();
    sha.UpdateData(t1, 16);
    sha.Finalize();
    for (int i = 0; i < 20; ++i)
    {
        vK[i * 2] = sha.GetDigest()[i];
    }
    for (int i = 0; i < 16; ++i)
    {
        t1[i] = t[i * 2 + 1];
    }
    sha.Initialize();
    sha.UpdateData(t1, 16);
    sha.Finalize();
    for (int i = 0; i < 20; ++i)
    {
        vK[i * 2 + 1] = sha.GetDigest()[i];
    }
    K.SetBinary(vK, 40);

    uint8 hash[20];

    sha.Initialize();
    sha.UpdateBigNumbers(&N, nullptr);
    sha.Finalize();
    memcpy(hash, sha.GetDigest(), 20);
    sha.Initialize();
    sha.UpdateBigNumbers(&g, nullptr);
    sha.Finalize();
    for (int i = 0; i < 20; ++i)
    {
        hash[i] ^= sha.GetDigest()[i];
    }
    BigNumber t3;
    t3.SetBinary(hash, 20);

    sha.Initialize();
    sha.UpdateData(_login);
    sha.Finalize();
    uint8 t4[SHA_DIGEST_LENGTH];
    memcpy(t4, sha.GetDigest(), SHA_DIGEST_LENGTH);

    sha.Initialize();
    sha.UpdateBigNumbers(&t3, nullptr);
    sha.UpdateData(t4, SHA_DIGEST_LENGTH);
    sha.UpdateBigNumbers(&s, &A, &B, &K, nullptr);
    sha.Finalize();
    BigNumber M;
    M.SetBinary(sha.GetDigest(), 20);

    ///- Check PIN data is correct
    bool pinResult = true;

    if (promptPin && !lp.securityFlags)
        pinResult = false; // expected PIN data but did not receive it

    if (promptPin && lp.securityFlags)
    {
        if ((lockFlags & FIXED_PIN) == FIXED_PIN)
        {
            pinResult = VerifyPinData(std::stoi(securityInfo), pinData);
            BASIC_LOG("[AuthChallenge] Account '%s' using IP '%s' PIN result: %u", _login.c_str(), get_remote_address().c_str(), pinResult);
        }
        else if ((lockFlags & TOTP) == TOTP)
        {
            for (int i = -2; i != 2; ++i)
            {
                auto pin = GenerateTotpPin(securityInfo, i);

                if (pin == uint32(-1))
                    break;

                if ((pinResult = VerifyPinData(pin, pinData)))
                    break;
            }
        }
        else if (_geoUnlockPIN)
        {
            pinResult = VerifyPinData(_geoUnlockPIN, pinData);
        }
        else
        {
            pinResult = false;
            sLog.outError("[ERROR] Invalid PIN flags set for user %s - user cannot log-in until fixed", _login.c_str());
        }
    }

    ///- Check if SRP6 results match (password is correct), else send an error
    if (!memcmp(M.AsByteArray().data(), lp.M1, 20) && pinResult)
    {
        if (!VerifyVersion(lp.A, sizeof(lp.A), lp.crc_hash, false))
        {
            BASIC_LOG("[AuthChallenge] Account %s tried to login with modified client!", _login.c_str());
            char data[2] = { CMD_AUTH_LOGON_PROOF, WOW_FAIL_VERSION_INVALID };
            send(data, sizeof(data));
            return true;
        }

        // Geolocking checks must be done after an otherwise successful login to prevent lockout attacks
        if (_geoUnlockPIN) // remove the PIN to unlock the account since login succeeded
        {
            auto result = LoginDatabase.PExecute("UPDATE `account` SET `geolock_pin` = 0 WHERE `username` = '%s'",
                _safelogin.c_str());

            if (!result)
            {
                sLog.outError("Unable to remove geolock PIN for %s - account has not been unlocked", _safelogin.c_str());
            }
        }
        else if (GeographicalLockCheck())
        {
            BASIC_LOG("Account '%s' (%u) using IP '%s' has been geolocked", _login.c_str(), _accountId, get_remote_address().c_str()); // todo, add additional logging info

            auto pin = urand(100000, 999999); // check rand32_max
            auto result = LoginDatabase.PExecute("UPDATE `account` SET `geolock_pin` = %u WHERE `username` = '%s'",
                pin, _safelogin.c_str());

            if (!result)
            {
                sLog.outError("Unable to write geolock PIN for %s - account has not been locked", _safelogin.c_str());

                char data[2] = { CMD_AUTH_LOGON_PROOF, WOW_FAIL_DB_BUSY };
                send(data, sizeof(data));
                return true;
            }

#ifdef USE_SENDGRID
            if (sConfig.GetBoolDefault("SendMail", false))
            {
                auto mail = std::make_unique<SendgridMail>
                (
                    sConfig.GetStringDefault("SendGridKey", ""),
                    sConfig.GetStringDefault("GeolockGUID", "")
                );

                mail->recipient(_email);
                mail->from(sConfig.GetStringDefault("MailFrom", ""));
                mail->substitution("%username%", _login);
                mail->substitution("%unlock_pin%", std::to_string(pin));
                mail->substitution("%originating_ip%", get_remote_address());

                MailerService::get_global_mailer()->send(std::move(mail),
                    [](SendgridMail::Result res)
                    {
                        DEBUG_LOG("Mail result: %d", res);
                    }
                );
            }
#endif

            char data[2] = { CMD_AUTH_LOGON_PROOF, WOW_FAIL_PARENTCONTROL };
            send(data, sizeof(data));
            return true;
        }

        BASIC_LOG("[AuthChallenge] Account '%s' using IP '%s' successfully authenticated", _login.c_str(), get_remote_address().c_str());

        ///- Update the sessionkey, last_ip, last login time and reset number of failed logins in the account table for this account
        // No SQL injection (escaped user name) and IP address as received by socket
        const char* K_hex = K.AsHexStr();
        const char *os = reinterpret_cast<char *>(&_os);    // no injection as there are only two possible values
        auto result = LoginDatabase.PQuery("UPDATE `account` SET `sessionkey` = '%s', `last_ip` = '%s', `last_login` = NOW(), `locale` = '%u', `failed_logins` = 0, `os` = '%s' WHERE `username` = '%s'",
            K_hex, get_remote_address().c_str(), GetLocaleByName(_localizationName), os, _safelogin.c_str() );
        delete result;
        OPENSSL_free((void*)K_hex);

        ///- Finish SRP6 and send the final result to the client
        sha.Initialize();
        sha.UpdateBigNumbers(&A, &M, &K, nullptr);
        sha.Finalize();

        SendProof(sha);

        ///- Set _status to authed!
        _status = STATUS_AUTHED;
    }
    else
    {
        if (_build > 6005)                                  // > 1.12.2
        {
            char data[4] = { CMD_AUTH_LOGON_PROOF, WOW_FAIL_UNKNOWN_ACCOUNT, 0, 0};
            send(data, sizeof(data));
        }
        else
        {
            // 1.x not react incorrectly at 4-byte message use 3 as real error
            char data[2] = { CMD_AUTH_LOGON_PROOF, WOW_FAIL_UNKNOWN_ACCOUNT};
            send(data, sizeof(data));
        }
        BASIC_LOG("[AuthChallenge] Account '%s' using IP '%s' tried to login with wrong password!", _login.c_str (), get_remote_address().c_str());

        uint32 MaxWrongPassCount = sConfig.GetIntDefault("WrongPass.MaxCount", 0);
        if(MaxWrongPassCount > 0)
        {
            //Increment number of failed logins by one and if it reaches the limit temporarily ban that account or IP
            LoginDatabase.PExecute("UPDATE `account` SET `failed_logins` = `failed_logins` + 1 WHERE `username` = '%s'",_safelogin.c_str());

            if(QueryResult *loginfail = LoginDatabase.PQuery("SELECT `id`, `failed_logins` FROM `account` WHERE `username` = '%s'", _safelogin.c_str()))
            {
                Field* fields = loginfail->Fetch();
                uint32 failed_logins = fields[1].GetUInt32();

                if( failed_logins >= MaxWrongPassCount )
                {
                    uint32 WrongPassBanTime = sConfig.GetIntDefault("WrongPass.BanTime", 600);
                    bool WrongPassBanType = sConfig.GetBoolDefault("WrongPass.BanType", false);

                    if(WrongPassBanType)
                    {
                        uint32 acc_id = fields[0].GetUInt32();
                        LoginDatabase.PExecute("INSERT INTO `account_banned` (`id`, `bandate`, `unbandate`, `bannedby`, `banreason`, `active`, `realm`) "
                            "VALUES ('%u',UNIX_TIMESTAMP(),UNIX_TIMESTAMP()+'%u','MaNGOS realmd','Failed login autoban',1,1)",
                            acc_id, WrongPassBanTime);
                        BASIC_LOG("[AuthChallenge] Account '%s' using  IP '%s' got banned for '%u' seconds because it failed to authenticate '%u' times",
                            _login.c_str(), get_remote_address().c_str(), WrongPassBanTime, failed_logins);
                    }
                    else
                    {
                        std::string current_ip = get_remote_address();
                        LoginDatabase.escape_string(current_ip);
                        LoginDatabase.PExecute("INSERT INTO `ip_banned` VALUES ('%s',UNIX_TIMESTAMP(),UNIX_TIMESTAMP()+'%u','MaNGOS realmd','Failed login autoban')",
                            current_ip.c_str(), WrongPassBanTime);
                        BASIC_LOG("[AuthChallenge] IP '%s' got banned for '%u' seconds because account '%s' failed to authenticate '%u' times",
                            current_ip.c_str(), WrongPassBanTime, _login.c_str(), failed_logins);
                    }
                }
                delete loginfail;
            }
        }
    }
    return true;
}

/// Reconnect Challenge command handler
bool AuthSocket::_HandleReconnectChallenge()
{
    DEBUG_LOG("Entering _HandleReconnectChallenge");
    if (recv_len() < sizeof(sAuthLogonChallenge_C))
        return false;

    ///- Read the first 4 bytes (header) to get the length of the remaining of the packet
    std::vector<uint8> buf;
    buf.resize(4);

    recv((char *)&buf[0], 4);

    EndianConvert(*((uint16*)(&buf[0])));
    uint16 remaining = ((sAuthLogonChallenge_C *)&buf[0])->size;
    DEBUG_LOG("[ReconnectChallenge] got header, body is %#04x bytes", remaining);

    if ((remaining < sizeof(sAuthLogonChallenge_C) - buf.size()) || (recv_len() < remaining))
        return false;

    ///- Session is closed unless overriden
    _status = STATUS_CLOSED;

    //No big fear of memory outage (size is int16, i.e. < 65536)
    buf.resize(remaining + buf.size() + 1);
    buf[buf.size() - 1] = 0;
    sAuthLogonChallenge_C *ch = (sAuthLogonChallenge_C*)&buf[0];

    ///- Read the remaining of the packet
    recv((char *)&buf[4], remaining);
    DEBUG_LOG("[ReconnectChallenge] got full packet, %#04x bytes", ch->size);
    DEBUG_LOG("[ReconnectChallenge] name(%d): '%s'", ch->I_len, ch->I);

    _login = (const char*)ch->I;

    _safelogin = _login;
    LoginDatabase.escape_string(_safelogin);

    EndianConvert(ch->build);
    _build = ch->build;

    QueryResult *result = LoginDatabase.PQuery ("SELECT `sessionkey`, `id` FROM `account` WHERE `username` = '%s'", _safelogin.c_str ());

    // Stop if the account is not found
    if (!result)
    {
        sLog.outError("[ERROR] user %s tried to login and we cannot find his session key in the database.", _login.c_str());
        close_connection();
        return false;
    }

    Field* fields = result->Fetch ();
    K.SetHexStr (fields[0].GetString ());
    _accountId = fields[1].GetUInt32();
    delete result;

    ///- All good, await client's proof
    _status = STATUS_RECON_PROOF;

    ///- Sending response
    ByteBuffer pkt;
    pkt << (uint8)  CMD_AUTH_RECONNECT_CHALLENGE;
    pkt << (uint8)  0x00;
    _reconnectProof.SetRand(16 * 8);
    pkt.append(_reconnectProof.AsByteArray(16));            // 16 bytes random
    pkt.append(VersionChallenge.data(), VersionChallenge.size());
    send((char const*)pkt.contents(), pkt.size());
    return true;
}

/// Reconnect Proof command handler
bool AuthSocket::_HandleReconnectProof()
{
    DEBUG_LOG("Entering _HandleReconnectProof");
    ///- Read the packet
    sAuthReconnectProof_C lp;
    if(!recv((char *)&lp, sizeof(sAuthReconnectProof_C)))
        return false;

    ///- Session is closed unless overriden
    _status = STATUS_CLOSED;

    if (_login.empty() || !_reconnectProof.GetNumBytes() || !K.GetNumBytes())
        return false;

    BigNumber t1;
    t1.SetBinary(lp.R1, 16);

    Sha1Hash sha;
    sha.Initialize();
    sha.UpdateData(_login);
    sha.UpdateBigNumbers(&t1, &_reconnectProof, &K, nullptr);
    sha.Finalize();

    if (!memcmp(sha.GetDigest(), lp.R2, SHA_DIGEST_LENGTH))
    {
        if (!VerifyVersion(lp.R1, sizeof(lp.R1), lp.R3, true))
        {
            ByteBuffer pkt;
            pkt << uint8(CMD_AUTH_RECONNECT_PROOF);
            pkt << uint8(WOW_FAIL_VERSION_INVALID);
            send((char const*)pkt.contents(), pkt.size());
            return true;
        }

        ///- Sending response
        ByteBuffer pkt;
        pkt << uint8(CMD_AUTH_RECONNECT_PROOF);
        pkt << uint8(WOW_SUCCESS);
        send((char const*)pkt.contents(), pkt.size());

        ///- Set _status to authed!
        _status = STATUS_AUTHED;

        return true;
    }
    else
    {
        sLog.outError("[ERROR] user %s tried to login, but session invalid.", _login.c_str());
        close_connection();
        return false;
    }
}

/// %Realm List command handler
bool AuthSocket::_HandleRealmList()
{
    DEBUG_LOG("Entering _HandleRealmList");
    if (recv_len() < 5)
        return false;

    recv_skip(5);

    // this shouldn't be possible, but just in case
    if (!_accountId)
        return false;

    // check for too frequent requests
    auto const minDelay = sConfig.GetIntDefault("MinRealmListDelay", 1);
    auto const now = time(nullptr);
    auto const delay = now - _lastRealmListRequest;

    _lastRealmListRequest = now;

    if (delay < minDelay)
    {
        sLog.outError("[ERROR] user %s IP %s is sending CMD_REALM_LIST too frequently.  Delay = %d seconds", _login.c_str(), get_remote_address().c_str(), delay);
        return false;
    }

    ///- Update realm list if need
    sRealmList.UpdateIfNeed();

    ///- Circle through realms in the RealmList and construct the return packet (including # of user characters in each realm)
    ByteBuffer pkt;
    LoadRealmlist(pkt);

    ByteBuffer hdr;
    hdr << (uint8) CMD_REALM_LIST;
    hdr << (uint16)pkt.size();
    hdr.append(pkt);

    send((char const*)hdr.contents(), hdr.size());

    return true;
}

void AuthSocket::LoadRealmlist(ByteBuffer &pkt)
{
    if (_build < 6299)        // before version 2.0.3 (exclusive)
    {
        pkt << uint32(0);                               // unused value
        pkt << uint8(sRealmList.size());

        for (RealmList::RealmMap::const_iterator i = sRealmList.begin(); i != sRealmList.end(); ++i)
        {
            uint8 AmountOfCharacters;

            // No SQL injection. id of realm is controlled by the database.
            QueryResult *result = LoginDatabase.PQuery("SELECT `numchars` FROM `realmcharacters` WHERE `realmid` = '%d' AND `acctid`='%u'", i->second.m_ID, _accountId);
            if (result)
            {
                Field *fields = result->Fetch();
                AmountOfCharacters = fields[0].GetUInt8();
                delete result;
            }
            else
                AmountOfCharacters = 0;

            bool ok_build = std::find(i->second.realmbuilds.begin(), i->second.realmbuilds.end(), _build) != i->second.realmbuilds.end();

            RealmBuildInfo const* buildInfo = ok_build ? FindBuildInfo(_build) : nullptr;
            if (!buildInfo)
                buildInfo = &i->second.realmBuildInfo;

            RealmFlags realmflags = i->second.realmflags;

            // 1.x clients not support explicitly REALM_FLAG_SPECIFYBUILD, so manually form similar name as show in more recent clients
            std::string name = i->first;
            if (realmflags & REALM_FLAG_SPECIFYBUILD)
            {
                char buf[20];
                snprintf(buf, 20, " (%u,%u,%u)", buildInfo->major_version, buildInfo->minor_version, buildInfo->bugfix_version);
                name += buf;
            }

            // Show offline state for unsupported client builds and locked realms (1.x clients not support locked state show)
            if (!ok_build || (i->second.allowedSecurityLevel > GetSecurityOn(i->second.m_ID)))
                realmflags = RealmFlags(realmflags | REALM_FLAG_OFFLINE);

            pkt << uint32(i->second.icon);              // realm type
            pkt << uint8(realmflags);                   // realmflags
            pkt << name;                                // name
            pkt << i->second.address;                   // address
            pkt << float(i->second.populationLevel);
            pkt << uint8(AmountOfCharacters);
            pkt << uint8(i->second.timezone);           // realm category
            pkt << uint8(0x00);                         // unk, may be realm number/id?
        }

        pkt << uint16(0x0002);                          // unused value (why 2?)
    }
    else
    {
        pkt << uint32(0);                               // unused value
        pkt << uint16(sRealmList.size());

        for (RealmList::RealmMap::const_iterator i = sRealmList.begin(); i != sRealmList.end(); ++i)
        {
            uint8 AmountOfCharacters;

            // No SQL injection. id of realm is controlled by the database.
            QueryResult *result = LoginDatabase.PQuery("SELECT `numchars` FROM `realmcharacters` WHERE `realmid` = '%d' AND `acctid`='%u'", i->second.m_ID, _accountId);
            if (result)
            {
                Field *fields = result->Fetch();
                AmountOfCharacters = fields[0].GetUInt8();
                delete result;
            }
            else
                AmountOfCharacters = 0;

            bool ok_build = std::find(i->second.realmbuilds.begin(), i->second.realmbuilds.end(), _build) != i->second.realmbuilds.end();

            RealmBuildInfo const* buildInfo = ok_build ? FindBuildInfo(_build) : nullptr;
            if (!buildInfo)
                buildInfo = &i->second.realmBuildInfo;

            uint8 lock = (i->second.allowedSecurityLevel > GetSecurityOn(i->second.m_ID)) ? 1 : 0;

            RealmFlags realmFlags = i->second.realmflags;

            // Show offline state for unsupported client builds
            if (!ok_build)
                realmFlags = RealmFlags(realmFlags | REALM_FLAG_OFFLINE);

            if (!buildInfo)
                realmFlags = RealmFlags(realmFlags & ~REALM_FLAG_SPECIFYBUILD);

            pkt << uint8(i->second.icon);               // realm type (this is second column in Cfg_Configs.dbc)
            pkt << uint8(lock);                         // flags, if 0x01, then realm locked
            pkt << uint8(realmFlags);                   // see enum RealmFlags
            pkt << i->first;                            // name
            pkt << i->second.address;                   // address
            pkt << float(i->second.populationLevel);
            pkt << uint8(AmountOfCharacters);
            pkt << uint8(i->second.timezone);           // realm category (Cfg_Categories.dbc)
            pkt << uint8(0x2C);                         // unk, may be realm number/id?

            if (realmFlags & REALM_FLAG_SPECIFYBUILD)
            {
                pkt << uint8(buildInfo->major_version);
                pkt << uint8(buildInfo->minor_version);
                pkt << uint8(buildInfo->bugfix_version);
                pkt << uint16(_build);
            }
        }

        pkt << uint16(0x0010);                          // unused value (why 10?)
    }
}

/// Resume patch transfer
bool AuthSocket::_HandleXferResume()
{
    DEBUG_LOG("Entering _HandleXferResume");

    if(recv_len() < 9)
        return false;

    recv_skip(1);

    uint64 start_pos;
    recv((char *)&start_pos, 8);

    if(patch_ == ACE_INVALID_HANDLE)
    {
        close_connection();
        return false;
    }

    ACE_OFF_T file_size = ACE_OS::filesize(patch_);

    if(file_size == -1 || start_pos >= (uint64)file_size)
    {
        close_connection();
        return false;
    }

    if(ACE_OS::lseek(patch_, start_pos, SEEK_SET) == -1)
    {
        close_connection();
        return false;
    }

    InitPatch();

    return true;
}

/// Cancel patch transfer
bool AuthSocket::_HandleXferCancel()
{
    DEBUG_LOG("Entering _HandleXferCancel");

    recv_skip(1);
    close_connection();

    return true;
}

/// Accept patch transfer
bool AuthSocket::_HandleXferAccept()
{
    DEBUG_LOG("Entering _HandleXferAccept");

    recv_skip(1);

    InitPatch();

    return true;
}

/// Verify PIN entry data
bool AuthSocket::VerifyPinData(uint32 pin, const PINData& clientData)
{
    // remap the grid to match the client's layout
    std::vector<uint8> grid { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    std::vector<uint8> remappedGrid(grid.size());

    uint8* remappedIndex = remappedGrid.data();
    uint32 seed = gridSeed;

    for (size_t i = grid.size(); i > 0; --i)
    {
        auto remainder = seed % i;
        seed /= i;
        *remappedIndex = grid[remainder];

        size_t copySize = i;
        copySize -= remainder;
        --copySize;

        uint8* srcPtr = grid.data() + remainder + 1;
        uint8* dstPtr = grid.data() + remainder;

        std::copy(srcPtr, srcPtr + copySize, dstPtr);
        ++remappedIndex;
    }

    // convert the PIN to bytes (for ex. '1234' to {1, 2, 3, 4})
    std::vector<uint8> pinBytes;

    while (pin != 0)
    {
        pinBytes.push_back(pin % 10);
        pin /= 10;
    }

    std::reverse(pinBytes.begin(), pinBytes.end());

    // validate PIN length
    if (pinBytes.size() < 4 || pinBytes.size() > 10)
        return false; // PIN outside of expected range

    // remap the PIN to calculate the expected client input sequence
    for (size_t i = 0; i < pinBytes.size(); ++i)
    {
        auto index = std::find(remappedGrid.begin(), remappedGrid.end(), pinBytes[i]);
        pinBytes[i] = std::distance(remappedGrid.begin(), index);
    }

    // convert PIN bytes to their ASCII values
    for (size_t i = 0; i < pinBytes.size(); ++i)
        pinBytes[i] += 0x30;

    // validate the PIN, x = H(client_salt | H(server_salt | ascii(pin_bytes)))
    Sha1Hash sha;
    sha.UpdateData(serverSecuritySalt.AsByteArray());
    sha.UpdateData(pinBytes.data(), pinBytes.size());
    sha.Finalize();

    BigNumber hash, clientHash;
    hash.SetBinary(sha.GetDigest(), sha.GetLength());
    clientHash.SetBinary(clientData.hash, 20);

    sha.Initialize();
    sha.UpdateData(clientData.salt, sizeof(clientData.salt));
    sha.UpdateData(hash.AsByteArray());
    sha.Finalize();
    hash.SetBinary(sha.GetDigest(), sha.GetLength());

    return !memcmp(hash.AsDecStr(), clientHash.AsDecStr(), 20);
}

uint32 AuthSocket::GenerateTotpPin(const std::string& secret, int interval) {
    std::vector<uint8> decoded_key((secret.size() + 7) / 8 * 5);
    int key_size = base32_decode((const uint8_t*)secret.data(), decoded_key.data(), decoded_key.size());

    if (key_size == -1)
    {
        DEBUG_LOG("Unable to base32 decode TOTP key for user %s", _safelogin.c_str());
        return -1;
    }

    // not guaranteed by the standard to be the UNIX epoch but it is on all supported platforms
    auto time = std::time(nullptr);
    uint64 now = static_cast<uint64>(time);
    uint64 step = static_cast<uint64>((floor(now / 30))) + interval;
    EndianConvertReverse(step);

    HmacHash hmac(decoded_key.data(), key_size);
    hmac.UpdateData((uint8*)&step, sizeof(step));
    hmac.Finalize();

    auto hmac_result = hmac.GetDigest();

    unsigned int offset = hmac_result[19] & 0xF;
    std::uint32_t pin = (hmac_result[offset] & 0x7f) << 24 | (hmac_result[offset + 1] & 0xff) << 16
        | (hmac_result[offset + 2] & 0xff) << 8 | (hmac_result[offset + 3] & 0xff);
    EndianConvert(pin);

    pin &= 0x7FFFFFFF;
    pin %= 1000000;
    return pin;
}

void AuthSocket::InitPatch()
{
    PatchHandler* handler = new PatchHandler(ACE_OS::dup(get_handle()), patch_);

    patch_ = ACE_INVALID_HANDLE;

    if(handler->open() == -1)
    {
        handler->close();
        close_connection();
    }
}

void AuthSocket::LoadAccountSecurityLevels(uint32 accountId)
{
    QueryResult* result = LoginDatabase.PQuery("SELECT `gmlevel`, `RealmID` FROM `account_access` WHERE `id` = %u",
        accountId);
    if (!result)
        return;

    do
    {
        Field *fields = result->Fetch();
        AccountTypes security = AccountTypes(fields[0].GetUInt32());
        int realmId = fields[1].GetInt32();
        if (realmId < 0)
            _accountDefaultSecurityLevel = security;
        else
            _accountSecurityOnRealm[realmId] = security;
    } while (result->NextRow());

    delete result;
}

bool AuthSocket::GeographicalLockCheck()
{
    if (!sConfig.GetBoolDefault("GeoLocking"), false)
    {
        return false;
    }

    if (_lastIP.empty() || _lastIP == get_remote_address())
    {
        return false;
    }

    if ((lockFlags & GEO_CITY) == 0 && (lockFlags & GEO_COUNTRY) == 0)
    {
        return false;
    }

    auto result = std::unique_ptr<QueryResult>(LoginDatabase.PQuery(
        "SELECT INET_ATON('%s') AS ip, network_start_integer, geoname_id, registered_country_geoname_id "
        "FROM geoip "
        "WHERE network_last_integer >= INET_ATON('%s') "
        "ORDER BY network_last_integer ASC LIMIT 1",
        get_remote_address().c_str(), get_remote_address().c_str())
        );

    auto result_prev = std::unique_ptr<QueryResult>(LoginDatabase.PQuery(
        "SELECT INET_ATON('%s') AS ip, network_start_integer, geoname_id, registered_country_geoname_id "
        "FROM geoip "
        "WHERE network_last_integer >= INET_ATON('%s') "
        "ORDER BY network_last_integer ASC LIMIT 1",
        _lastIP.c_str(), _lastIP.c_str())
        );

    if (!result && !result_prev)
    {
        return false;
    }

    // If only one of the queries returns a result, assume location has changed
    if ((result && !result_prev) || (!result && result_prev))
    {
        return true;
    }

    uint32_t net_start = result->Fetch()[1].GetUInt32();
    uint32_t net_start_prev = result_prev->Fetch()[1].GetUInt32();
    uint32_t ip = result->Fetch()[0].GetUInt32();
    uint32_t ip_prev = result_prev->Fetch()[0].GetUInt32();

    /* The optimised query will return the next highest range in the event
     * of the address not being found in the database. Therefore, we need
     * to perform a second check to ensure our address falls within
     * the returned range.
     * See: https://blog.jcole.us/2007/11/24/on-efficiently-geo-referencing-ips-with-maxmind-geoip-and-mysql-gis/
     */
    if (net_start > ip || net_start_prev > ip_prev)
    {
        return false;
    }

    std::string geoname_id = result->Fetch()[2].GetString();
    std::string country_geoname_id = result->Fetch()[3].GetString();
    std::string prev_geoname_id = result_prev->Fetch()[2].GetString();
    std::string prev_country_geoname_id = result_prev->Fetch()[3].GetString();

    if (lockFlags & GEO_CITY)
    {
        return geoname_id != prev_geoname_id;
    }
    else
    {
        return country_geoname_id != prev_country_geoname_id;
    }
}

bool AuthSocket::VerifyVersion(uint8 const* a, int32 aLength, uint8 const* versionProof, bool isReconnect)
{
    if (!sConfig.GetBoolDefault("StrictVersionCheck", false))
        return true;

    std::array<uint8, 20> zeros = { {} };
    std::array<uint8, 20> const* versionHash = nullptr;
    if (!isReconnect)
    {
        if (!((_platform == X86 || _platform == PPC) && (_os == Win || _os == OSX)))
            return false;

        RealmBuildInfo const* buildInfo = FindBuildInfo(_build);
        if (!buildInfo)
            return false;

        if (_os == Win)
            versionHash = &buildInfo->WindowsHash;
        else if (_os == OSX)
            versionHash = &buildInfo->MacHash;

        if (!versionHash)
            return false;

        if (!memcmp(versionHash->data(), zeros.data(), zeros.size()))
            return true;                                                            // not filled serverside
    }
    else
        versionHash = &zeros;

    Sha1Hash version;
    version.UpdateData(a, aLength);
    version.UpdateData(versionHash->data(), versionHash->size());
    version.Finalize();

    return memcmp(versionProof, version.GetDigest(), version.GetLength()) == 0;
}
