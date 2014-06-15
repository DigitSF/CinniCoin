// Copyright (c) 2014 The CinniCoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/*
Notes:
    Running with -debug could leave to and from address hashes and public keys in the log.
    
    
    parameters:
        -nosmsg             Disable secure messaging (fNoSmsg)
        -debugsmsg          Show extra debug messages (fDebugSmsg)
        -smsgscanchain      Scan the block chain for public key addresses on startup
    
    
*/

#include "emessage.h"

#include <stdint.h>
#include <time.h>
#include <map>
#include <stdexcept>
#include <sstream>
#include <errno.h>

#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/ecdh.h>
#include <openssl/sha.h>
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <boost/lexical_cast.hpp>


#include "base58.h"
#include "db.h"
#include "init.h" // pwalletMain


#include "lz4/lz4.c"

#include "xxhash/xxhash.h"
#include "xxhash/xxhash.c"


// On 64 bit system ld is 64bits
// set IS_ARCH_64 in makefile
#ifdef IS_ARCH_64
#undef PRI64d
#undef PRI64u
#undef PRI64x
#define PRI64d  "ld"
#define PRI64u  "lu"
#define PRI64x  "lx"
#endif // IS_ARCH_64


// TODO: For buckets older than current, only need to store no. messages and hash in memory

boost::signals2::signal<void (SecInboxMsg& inboxHdr)> NotifySecMsgInboxChanged;
boost::signals2::signal<void (SecOutboxMsg& outboxHdr)> NotifySecMsgOutboxChanged;

bool fSecMsgEnabled = false;

std::map<int64_t, SecMsgBucket> smsgSets;
uint32_t nPeerIdCounter = 1;


CCriticalSection cs_smsg;       // all except inbox, outbox and sendQueue
CCriticalSection cs_smsgInbox;
CCriticalSection cs_smsgOutbox;
CCriticalSection cs_smsgSendQueue;


namespace fs = boost::filesystem;

bool SMsgCrypter::SetKey(const std::vector<unsigned char>& vchNewKey, unsigned char* chNewIV)
{
    // -- for EVP_aes_256_cbc() key must be 256 bit, iv must be 128 bit.
    memcpy(&chKey[0], &vchNewKey[0], sizeof(chKey));
    memcpy(chIV, chNewIV, sizeof(chIV));
    
    fKeySet = true;
    
    return true;
};

bool SMsgCrypter::Encrypt(unsigned char* chPlaintext, uint32_t nPlain, std::vector<unsigned char> &vchCiphertext)
{
    if (!fKeySet)
        return false;
    
    // -- max ciphertext len for a n bytes of plaintext is n + AES_BLOCK_SIZE - 1 bytes
    int nLen = nPlain;
    
    int nCLen = nLen + AES_BLOCK_SIZE, nFLen = 0;
    vchCiphertext = std::vector<unsigned char> (nCLen);

    EVP_CIPHER_CTX ctx;

    bool fOk = true;

    EVP_CIPHER_CTX_init(&ctx);
    if (fOk) fOk = EVP_EncryptInit_ex(&ctx, EVP_aes_256_cbc(), NULL, &chKey[0], &chIV[0]);
    if (fOk) fOk = EVP_EncryptUpdate(&ctx, &vchCiphertext[0], &nCLen, chPlaintext, nLen);
    if (fOk) fOk = EVP_EncryptFinal_ex(&ctx, (&vchCiphertext[0])+nCLen, &nFLen);
    EVP_CIPHER_CTX_cleanup(&ctx);

    if (!fOk)
        return false;

    vchCiphertext.resize(nCLen + nFLen);
    
    return true;
};

bool SMsgCrypter::Decrypt(unsigned char* chCiphertext, uint32_t nCipher, std::vector<unsigned char>& vchPlaintext)
{
    if (!fKeySet)
        return false;
    
    // plaintext will always be equal to or lesser than length of ciphertext
    int nPLen = nCipher, nFLen = 0;
    
    vchPlaintext.resize(nCipher);

    EVP_CIPHER_CTX ctx;

    bool fOk = true;

    EVP_CIPHER_CTX_init(&ctx);
    if (fOk) fOk = EVP_DecryptInit_ex(&ctx, EVP_aes_256_cbc(), NULL, &chKey[0], &chIV[0]);
    if (fOk) fOk = EVP_DecryptUpdate(&ctx, &vchPlaintext[0], &nPLen, &chCiphertext[0], nCipher);
    if (fOk) fOk = EVP_DecryptFinal_ex(&ctx, (&vchPlaintext[0])+nPLen, &nFLen);
    EVP_CIPHER_CTX_cleanup(&ctx);

    if (!fOk)
        return false;
    
    vchPlaintext.resize(nPLen + nFLen);
    
    return true;
};

void SecMsgBucket::hashBucket()
{
    if (fDebugSmsg)
        printf("SecMsgBucket::hashBucket()\n");
    
    timeChanged = GetTime();
    
    std::set<SecMsgToken>::iterator it;
    
    void* state = XXH32_init(1);
    
    for (it = setTokens.begin(); it != setTokens.end(); ++it)
    {
        XXH32_update(state, it->sample, 8);
    };
    
    hash = XXH32_digest(state);
    
    if (fDebugSmsg)
        printf("Hashed %"PRIszu" messages, hash %u\n", setTokens.size(), hash);
};

bool CSmesgInboxDB::NextSmesg(Dbc* pcursor, unsigned int fFlags, std::vector<unsigned char>& vchKey, SecInboxMsg& smsgInbox)
{
    datKey.set_flags(DB_DBT_USERMEM);
    datValue.set_flags(DB_DBT_USERMEM);
    
    
    datKey.set_ulen(vchKeyData.size());
    datKey.set_data(&vchKeyData[0]);

    datValue.set_ulen(vchValueData.size());
    datValue.set_data(&vchValueData[0]);
    
    while (true) // Must loop, as want to return only message keys
    {
        int ret = pcursor->get(&datKey, &datValue, fFlags);
        
        if (ret == ENOMEM
         || ret == DB_BUFFER_SMALL)
        {
            if (datKey.get_size() > datKey.get_ulen())
            {
                vchKeyData.resize(datKey.get_size());
                datKey.set_ulen(vchKeyData.size());
                datKey.set_data(&vchKeyData[0]);
            };

            if (datValue.get_size() > datValue.get_ulen())
            {
                vchValueData.resize(datValue.get_size());
                datValue.set_ulen(vchValueData.size());
                datValue.set_data(&vchValueData[0]);
            };
            // -- try once more, when DB_BUFFER_SMALL cursor is not expected to move
            ret = pcursor->get(&datKey, &datValue, fFlags);
        };

        if (ret == DB_NOTFOUND)
            return false;
        else
        if (datKey.get_data() == NULL || datValue.get_data() == NULL || ret != 0)
        {
            printf("CSmesgInboxDB::NextSmesg(), DB error %d, %s\n", ret, db_strerror(ret));
            return false;
        };

        if (datKey.get_size() != 17)
        {
            fFlags = DB_NEXT; // don't want to loop forever
            continue; // not a message key
        }
        // must be a better way?
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.SetType(SER_DISK);
        ssValue.clear();
        ssValue.write((char*)datKey.get_data(), datKey.get_size());
        ssValue >> vchKey;
        //SecOutboxMsg smsgOutbox;
        ssValue.clear();
        ssValue.write((char*)datValue.get_data(), datValue.get_size());
        ssValue >> smsgInbox;
        break;
    }
    
    return true;
};


bool CSmesgOutboxDB::NextSmesg(Dbc* pcursor, unsigned int fFlags, std::vector<unsigned char>& vchKey, SecOutboxMsg& smsgOutbox)
{
    datKey.set_flags(DB_DBT_USERMEM);
    datValue.set_flags(DB_DBT_USERMEM);
    
    
    datKey.set_ulen(vchKeyData.size());
    datKey.set_data(&vchKeyData[0]);

    datValue.set_ulen(vchValueData.size());
    datValue.set_data(&vchValueData[0]);
    
    while (true) // Must loop, as want to return only message keys
    {
        int ret = pcursor->get(&datKey, &datValue, fFlags);
        
        if (ret == ENOMEM
         || ret == DB_BUFFER_SMALL)
        {
            if (datKey.get_size() > datKey.get_ulen())
            {
                vchKeyData.resize(datKey.get_size());
                datKey.set_ulen(vchKeyData.size());
                datKey.set_data(&vchKeyData[0]);
            };

            if (datValue.get_size() > datValue.get_ulen())
            {
                vchValueData.resize(datValue.get_size());
                datValue.set_ulen(vchValueData.size());
                datValue.set_data(&vchValueData[0]);
            };
            // try once more, when DB_BUFFER_SMALL cursor is not expected to move
            ret = pcursor->get(&datKey, &datValue, fFlags);
        };

        if (ret == DB_NOTFOUND)
            return false;
        else
        if (datKey.get_data() == NULL || datValue.get_data() == NULL || ret != 0)
        {
            printf("CSmesgOutboxDB::NextSmesg(), DB error %d, %s\n", ret, db_strerror(ret));
            return false;
        };

        if (datKey.get_size() != 17)
        {
            fFlags = DB_NEXT; // don't want to loop forever
            continue; // not a message key
        }
        // must be a better way?
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.SetType(SER_DISK);
        ssValue.clear();
        ssValue.write((char*)datKey.get_data(), datKey.get_size());
        ssValue >> vchKey;
        //SecOutboxMsg smsgOutbox;
        ssValue.clear();
        ssValue.write((char*)datValue.get_data(), datValue.get_size());
        ssValue >> smsgOutbox;
        break;
    }
    
    return true;
};

bool CSmesgSendQueueDB::NextSmesg(Dbc* pcursor, unsigned int fFlags, std::vector<unsigned char>& vchKey, SecOutboxMsg& smsgOutbox)
{
    datKey.set_flags(DB_DBT_USERMEM);
    datValue.set_flags(DB_DBT_USERMEM);
    
    
    datKey.set_ulen(vchKeyData.size());
    datKey.set_data(&vchKeyData[0]);

    datValue.set_ulen(vchValueData.size());
    datValue.set_data(&vchValueData[0]);
    
    while (true) // Must loop, as want to return only message keys
    {
        int ret = pcursor->get(&datKey, &datValue, fFlags);
        
        if (ret == ENOMEM
         || ret == DB_BUFFER_SMALL)
        {
            if (datKey.get_size() > datKey.get_ulen())
            {
                vchKeyData.resize(datKey.get_size());
                datKey.set_ulen(vchKeyData.size());
                datKey.set_data(&vchKeyData[0]);
            };

            if (datValue.get_size() > datValue.get_ulen())
            {
                vchValueData.resize(datValue.get_size());
                datValue.set_ulen(vchValueData.size());
                datValue.set_data(&vchValueData[0]);
            };
            // try once more, when DB_BUFFER_SMALL cursor is not expected to move
            ret = pcursor->get(&datKey, &datValue, fFlags);
        };

        if (ret == DB_NOTFOUND)
            return false;
        else
        if (datKey.get_data() == NULL || datValue.get_data() == NULL || ret != 0)
        {
            printf("CSmesgOutboxDB::NextSmesg(), DB error %d, %s\n", ret, db_strerror(ret));
            return false;
        };

        if (datKey.get_size() != 17)
        {
            fFlags = DB_NEXT; // don't want to loop forever
            continue; // not a message key
        }
        // must be a better way?
        CDataStream ssValue(SER_DISK, CLIENT_VERSION);
        ssValue.SetType(SER_DISK);
        ssValue.clear();
        ssValue.write((char*)datKey.get_data(), datKey.get_size());
        ssValue >> vchKey;
        //SecOutboxMsg smsgOutbox;
        ssValue.clear();
        ssValue.write((char*)datValue.get_data(), datValue.get_size());
        ssValue >> smsgOutbox;
        break;
    }
    
    return true;
};





void ThreadSecureMsg(void* parg)
{
    // -- bucket management thread
    RenameThread("CinniCoin-smsg"); // Make this thread recognisable
    
    uint32_t delay = 0;
    
    //while (!fShutdown)
    while (fSecMsgEnabled)
    {
        // shutdown thread waits 5 seconds, this should be less
        Sleep(1000); // milliseconds
        
        if (!fSecMsgEnabled) // check again after sleep
            break;
        
        delay++;
        if (delay < SMSG_THREAD_DELAY) // check every SMSG_THREAD_DELAY seconds
            continue;
        delay = 0;
        
        int64_t now = GetTime();
        
        if (fDebugSmsg)
            printf("SecureMsgThread %"PRI64d" \n", now);
        
        int64_t cutoffTime = now - SMSG_RETENTION;
        
        {
            LOCK(cs_smsg);
            std::map<int64_t, SecMsgBucket>::iterator it;
            it = smsgSets.begin();
            
            while (it != smsgSets.end())
            {
                //if (fDebugSmsg)
                //    printf("Checking bucket %"PRI64d", size %"PRIszu" \n", it->first, it->second.setTokens.size());
                if (it->first < cutoffTime)
                {
                    if (fDebugSmsg)
                        printf("Removing bucket %"PRI64d" \n", it->first);
                    std::string fileName = boost::lexical_cast<std::string>(it->first) + "_01.dat";
                    fs::path fullPath = GetDataDir() / "smsgStore" / fileName;
                    if (fs::exists(fullPath))
                    {
                        try {
                            fs::remove(fullPath);
                        } catch (const fs::filesystem_error& ex)
                        {
                            printf("Error removing bucket file %s.\n", ex.what());
                        };
                    } else
                        printf("Path %s does not exist \n", fullPath.string().c_str());
                    
                    smsgSets.erase(it++);
                } else
                {
                    // -- tick down nLockCount, so will eventually expire if peer never sends data
                    if (it->second.nLockCount > 0)
                    {
                        it->second.nLockCount--;
                        
                        if (it->second.nLockCount == 0)     // lock timed out
                        {
                            uint32_t    nPeerId     = it->second.nLockPeerId;
                            int64_t     ignoreUntil = GetTime() + SMSG_TIME_IGNORE;
                            
                            if (fDebugSmsg)
                                printf("Lock on bucket %"PRI64d" for peer %u timed out.\n", it->first, nPeerId);
                            // -- look through the nodes for the peer that locked this bucket
                            LOCK(cs_vNodes);
                            BOOST_FOREACH(CNode* pnode, vNodes)
                            {
                                if (pnode->smsgData.nPeerId != nPeerId)
                                    continue;
                                pnode->smsgData.ignoreUntil = ignoreUntil;
                                
                                // -- alert peer that they are being ignored
                                std::vector<unsigned char> vchData;
                                vchData.resize(8);
                                memcpy(&vchData[0], &ignoreUntil, 8);
                                pnode->PushMessage("smsgIgnore", vchData);
                                
                                if (fDebugSmsg)
                                    printf("This node will ignore peer %u until %"PRI64d".\n", nPeerId, ignoreUntil);
                                break;
                            };
                            it->second.nLockPeerId = 0;
                        }; // if (it->second.nLockCount == 0)
                    };
                    ++it;
                }; // ! if (it->first < cutoffTime)
            };
        }; // LOCK(cs_smsg);
        
    };
    
    printf("ThreadSecureMsg exited.\n");
};

void ThreadSecureMsgPow(void* parg)
{
    // -- proof of work thread
    RenameThread("CinniCoin-smsg-pow"); // Make this thread recognisable
    
    int rv;
    std::vector<unsigned char> vchKey;
    SecOutboxMsg smsgOutbox;
    
    //while (!fShutdown)
    while (fSecMsgEnabled)
    {
        // sleep at end, then fShutdown is tested on wake
        {
            LOCK(cs_smsgSendQueue);
            
            // TODO: How to tell if db was opened successfully? For now create db
            CSmesgSendQueueDB dbSendQueue("cr+");
            
            // -- fifo
            unsigned int fFlags = DB_FIRST;
            for (;;)
            {
                Dbc* pcursor = dbSendQueue.GetAtCursor();
                
                if (!dbSendQueue.NextSmesg(pcursor, fFlags, vchKey, smsgOutbox))
                {
                    pcursor->close();
                    break;
                };
                
                if (fDebugSmsg)
                    printf("ThreadSecureMsgPow picked up a message to: %s.\n", smsgOutbox.sAddrTo.c_str());
                
                unsigned char* pHeader = &smsgOutbox.vchMessage[0];
                unsigned char* pPayload = &smsgOutbox.vchMessage[SMSG_HDR_LEN];
                SecureMessage* psmsg = (SecureMessage*) pHeader;
                
                // -- do proof of work
                if ((rv = SecureMsgSetHash(pHeader, pPayload, psmsg->nPayload)) != 0)
                {
                    // -- leave message in db, if terminated due to shutdown
                    pcursor->close();
                    
                    if (rv == 2)
                    {
                        break;
                    } else
                    {
                        printf("SecMsgPow: Could not get proof of work hash, message removed.\n");
                        dbSendQueue.EraseSmesg(vchKey);
                    };
                    continue;
                };
                
                
                // -- add to message store
                {
                    LOCK(cs_smsg);
                    if (SecureMsgStore(pHeader, pPayload, psmsg->nPayload, true) != 0)
                    {
                        printf("SecMsgPow: Could not place message in buckets, message removed.\n");
                        pcursor->close();
                        dbSendQueue.EraseSmesg(vchKey);
                        continue;
                    };
                }
                
                // -- test if message was sent to self
                if (SecureMsgScanMessage(pHeader, pPayload, psmsg->nPayload) != 0)
                {
                    // message recipient is not this node (or failed)
                };
                
                
                pcursor->close();
                
                dbSendQueue.EraseSmesg(vchKey);
                if (fDebugSmsg)
                    printf("ThreadSecureMsgPow() sent message to: %s.\n", smsgOutbox.sAddrTo.c_str());
                
                
                //break;
            };
            
        }
        
        // shutdown thread waits 5 seconds, this should be less
        Sleep(1000); // milliseconds
    };
    
    printf("ThreadSecureMsgPow exited.\n");
};


std::string getTimeString(int64_t timestamp, char *buffer, size_t nBuffer)
{
    struct tm* dt;
    time_t t = timestamp;
    dt = localtime(&t);
    
    strftime(buffer, nBuffer, "%Y-%m-%d %H:%M:%S %z", dt); // %Z shows long strings on windows
    return std::string(buffer); // Copies the null-terminated character sequence
};

std::string fsReadable(uint64_t nBytes)
{
    char buffer[128];
    if (nBytes >= 1024ll*1024ll*1024ll*1024ll)
        snprintf(buffer, sizeof(buffer), "%.2f TB", nBytes/1024.0/1024.0/1024.0/1024.0);
    else
    if (nBytes >= 1024*1024*1024)
        snprintf(buffer, sizeof(buffer), "%.2f GB", nBytes/1024.0/1024.0/1024.0);
    else
    if (nBytes >= 1024*1024)
        snprintf(buffer, sizeof(buffer), "%.2f MB", nBytes/1024.0/1024.0);
    else
    if (nBytes >= 1024)
        snprintf(buffer, sizeof(buffer), "%.2f KB", nBytes/1024.0);
    else
        snprintf(buffer, sizeof(buffer), "%"PRI64u" bytes", nBytes);
    return std::string(buffer);
};

int SecureMsgBuildBucketSet()
{
    /*
        Build the bucket set by scanning the files in the smsgStore dir.
        
        smsgSets should be empty
    */
    
    if (fDebugSmsg)
        printf("SecureMsgBuildBucketSet()\n");
        
    int64_t now = GetTime();
    uint32_t nFiles = 0;
    uint32_t nMessages = 0;
    
    fs::path pathSmsgDir = GetDataDir() / "smsgStore";
    fs::directory_iterator itend;
    
    
    if (fs::exists(pathSmsgDir)
        && fs::is_directory(pathSmsgDir))
    {
        for( fs::directory_iterator itd(pathSmsgDir) ; itd != itend ; ++itd)
        {
            if (!fs::is_regular_file(itd->status()))
                continue;
            
            std::string fileType = (*itd).path().extension().string();
            
            if (fileType.compare(".dat") != 0)
                continue;
                
            std::string fileName = (*itd).path().filename().string();
            
            if (fDebugSmsg)
                printf("Processing file: %s.\n", fileName.c_str());
            
            nFiles++;
            
            // TODO files must be split if > 2GB
            // time_noFile.dat
            size_t sep = fileName.find_last_of("_");
            if (sep == std::string::npos)
                continue;
            
            std::string stime = fileName.substr(0, sep);
            
            int64_t fileTime = boost::lexical_cast<int64_t>(stime);
            
            if (fileTime < now - SMSG_RETENTION)
            {
                printf("Dropping message set %"PRI64d".\n", fileTime);
                fs::remove((*itd).path());
                continue;
            };
            
            
            SecureMessage smsg;
            std::set<SecMsgToken>& tokenSet = smsgSets[fileTime].setTokens;
            
            {
                LOCK(cs_smsg);
                FILE *fp;
                
                if (!(fp = fopen((*itd).path().string().c_str(), "rb")))
                {
                    printf("Error opening file: %s\n", strerror(errno));
                    continue;
                };
                
                while (1)
                {
                    long int ofs = ftell(fp);
                    SecMsgToken token;
                    token.offset = ofs;
                    errno = 0;
                    if (fread(&smsg.hash[0], sizeof(unsigned char), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN)
                    {
                        if (errno != 0)
                        {
                            printf("fread header failed: %s\n", strerror(errno));
                        } else
                        {
                            //printf("End of file.\n");
                        };
                        break;
                    };
                    token.timestamp = smsg.timestamp;
                    
                    if (smsg.nPayload < 8)
                        continue;
                    
                    if (fread(token.sample, sizeof(unsigned char), 8, fp) != 8)
                    {
                        printf("fread data failed: %s\n", strerror(errno));
                        break;
                    };
                    
                    if (fseek(fp, smsg.nPayload-8, SEEK_CUR) != 0)
                    {
                        printf("fseek, strerror: %s.\n", strerror(errno));
                        break;
                    };
                    
                    tokenSet.insert(token);
                };
                
                fclose(fp);
            };
            smsgSets[fileTime].hashBucket();
            
            nMessages += tokenSet.size();
            
            if (fDebugSmsg)
                printf("Bucket %"PRI64d" contains %"PRIszu" messages.\n", fileTime, tokenSet.size());
        };
    };
    
    printf("Processed %u files, loaded %"PRIszu" buckets containing %u messages.\n", nFiles, smsgSets.size(), nMessages);
    
    return 0;
};

/** called from AppInit2() in init.cpp */
bool SecureMsgStart(bool fDontStart, bool fScanChain)
{
    if (fDontStart)
    {
        printf("Secure messaging not started.\n");
        return false;
    };
    
    printf("Secure messaging starting.\n");
    
    fSecMsgEnabled = true;
    
    if (fScanChain)
    {
        SecureMsgScanBlockChain();
    };
    
    if (SecureMsgBuildBucketSet() != 0)
    {
        printf("SecureMsg could not load bucket sets, secure messaging disabled.\n");
        fSecMsgEnabled = false;
        return false;
    };
    
    // -- start threads
    if (!NewThread(ThreadSecureMsg, NULL)
        || !NewThread(ThreadSecureMsgPow, NULL))
    {
        printf("SecureMsg could not start threads, secure messaging disabled.\n");
        fSecMsgEnabled = false;
        return false;
    };
    
    return true;
};

/** called from Shutdown() in init.cpp */
bool SecureMsgShutdown()
{
    if (!fSecMsgEnabled)
        return false;
    
    printf("Stopping secure messaging.\n");
    
    fSecMsgEnabled = false;
    // -- main program will wait 5 seconds for threads to terminate.
    
    return true;
};

bool SecureMsgEnable()
{
    // -- start secure messaging at runtime
    if (fSecMsgEnabled)
    {
        printf("SecureMsgEnable: secure messaging is already enabled.\n");
        return false;
    };
    
    
    {
        LOCK(cs_smsg);
        fSecMsgEnabled = true;
        
        smsgSets.clear(); // should be empty already
        
        if (SecureMsgBuildBucketSet() != 0)
        {
            printf("SecureMsgEnable: could not load bucket sets, secure messaging disabled.\n");
            fSecMsgEnabled = false;
            return false;
        };
        
    }; // LOCK(cs_smsg);
    
    // -- start threads
    if (!NewThread(ThreadSecureMsg, NULL)
        || !NewThread(ThreadSecureMsgPow, NULL))
    {
        printf("SecureMsgEnable could not start threads, secure messaging disabled.\n");
        fSecMsgEnabled = false;
        return false;
    };
    
    // -- ping each peer, don't know which have messaging enabled
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
        {
            pnode->PushMessage("smsgPing");
            pnode->PushMessage("smsgPong"); // Send pong as have missed initial ping sent by peer when it connected
        };
    }
    
    printf("Secure messaging enabled.\n");
    return true;
};

bool SecureMsgDisable()
{
    // -- stop secure messaging at runtime
    if (!fSecMsgEnabled)
    {
        printf("SecureMsgDisable: secure messaging is already disabled.\n");
        return false;
    };
    
    {
        LOCK(cs_smsg);
        fSecMsgEnabled = false;
        
        // -- clear smsgSets
        std::map<int64_t, SecMsgBucket>::iterator it;
        it = smsgSets.begin();
        for (it = smsgSets.begin(); it != smsgSets.end(); ++it)
        {
            it->second.setTokens.clear();
        };
        smsgSets.clear();
        
        // -- tell each smsg enabled peer that this node is disabling
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                if (!pnode->smsgData.fEnabled)
                    continue;
                
                pnode->PushMessage("smsgDisabled");
                pnode->smsgData.fEnabled = false;
            };
        }
    
    }; // LOCK(cs_smsg);
    
    // -- allow time for threads to stop
    Sleep(3000); // milliseconds
    // TODO be certain that threads have stopped
    
    
    printf("Secure messaging disabled.\n");
    return true;
};


bool SecureMsgReceiveData(CNode* pfrom, std::string strCommand, CDataStream& vRecv)
{
    /*
        Called from ProcessMessage
        Runs in ThreadMessageHandler2
    */
    
    if (fDebugSmsg)
        printf("SecureMsgReceiveData() %s %s.\n", pfrom->addrName.c_str(), strCommand.c_str());
    
    {
    // break up?
    LOCK(cs_smsg);
    
    if (strCommand == "smsgInv")
    {
        std::vector<unsigned char> vchData;
        vRecv >> vchData;
        
        if (vchData.size() < 4)
        {
            pfrom->Misbehaving(1);
            return false; // not enough data received to be a valid smsgInv
        };
        
        int64_t now = GetTime();
        
        if (now < pfrom->smsgData.ignoreUntil)
        {
            if (fDebugSmsg)
                printf("Node is ignoring peer %u until %"PRI64d".\n", pfrom->smsgData.nPeerId, pfrom->smsgData.ignoreUntil);
            return false;
        };
        
        uint32_t nBuckets       = smsgSets.size();
        uint32_t nLocked        = 0;    // no. of locked buckets on this node
        uint32_t nInvBuckets;           // no. of bucket headers sent by peer in smsgInv
        memcpy(&nInvBuckets, &vchData[0], 4);
        if (fDebugSmsg)
            printf("Remote node sent %d bucket headers, this has %d.\n", nInvBuckets, nBuckets);
        
        
        // -- Check no of buckets:
        if (nInvBuckets > (SMSG_RETENTION / SMSG_BUCKET_LEN) + 1) // +1 for some leeway
        {
            printf("Peer sent more bucket headers than possible %u, %u.\n", nInvBuckets, (SMSG_RETENTION / SMSG_BUCKET_LEN));
            pfrom->Misbehaving(1);
            return false;
        };
        
        if (vchData.size() < 4 + nInvBuckets*16)
        {
            printf("Remote node did not send enough data.\n");
            pfrom->Misbehaving(1);
            return false;
        };
        
        std::vector<unsigned char> vchDataOut;
        vchDataOut.reserve(4 + 8 * nInvBuckets); // reserve max possible size
        vchDataOut.resize(4);
        uint32_t nShowBuckets = 0;
        
        
        unsigned char *p = &vchData[4];
        for (uint32_t i = 0; i < nInvBuckets; ++i)
        {
            int64_t time;
            uint32_t ncontent, hash;
            memcpy(&time, p, 8);
            memcpy(&ncontent, p+8, 4);
            memcpy(&hash, p+12, 4);
            
            p += 16;
            
            // Check time valid:
            if (time < now - SMSG_RETENTION)
            {
                if (fDebugSmsg)
                    printf("Not interested in peer bucket %"PRI64d", has expired.\n", time);
                
                if (time < now - SMSG_RETENTION - SMSG_TIME_LEEWAY)
                    pfrom->Misbehaving(1);
                continue;
            };
            if (time > now + SMSG_TIME_LEEWAY)
            {
                if (fDebugSmsg)
                    printf("Not interested in peer bucket %"PRI64d", in the future.\n", time);
                pfrom->Misbehaving(1);
                continue;
            };
            
            if (ncontent < 1)
            {
                if (fDebugSmsg)
                    printf("Peer sent empty bucket, ignore %"PRI64d" %u %u.\n", time, ncontent, hash);
                continue;
            };
            
            if (fDebugSmsg)
            {
                printf("peer bucket %"PRI64d" %u %u.\n", time, ncontent, hash);
                printf("this bucket %"PRI64d" %"PRIszu" %u.\n", time, smsgSets[time].setTokens.size(), smsgSets[time].hash);
            };
            
            if (smsgSets[time].nLockCount > 0)
            {
                if (fDebugSmsg)
                    printf("Bucket is locked %u, waiting for peer %u to send data.\n", smsgSets[time].nLockCount, smsgSets[time].nLockPeerId);
                nLocked++;
                continue;
            };
            
            // -- if this node has more than the peer node, peer node will pull from this
            //    if then peer node has more this node will pull fom peer
            if (smsgSets[time].setTokens.size() < ncontent
                || (smsgSets[time].setTokens.size() == ncontent
                    && smsgSets[time].hash != hash)) // if same amount in buckets check hash
            {
                if (fDebugSmsg)
                    printf("Requesting contents of bucket %"PRI64d".\n", time);
                
                uint32_t sz = vchDataOut.size();
                vchDataOut.resize(sz + 8);
                memcpy(&vchDataOut[sz], &time, 8);
                
                nShowBuckets++;
            };
        };
        
        // TODO: should include hash?
        memcpy(&vchDataOut[0], &nShowBuckets, 4);
        if (vchDataOut.size() > 4)
        {
            pfrom->PushMessage("smsgShow", vchDataOut);
        } else
        if (nLocked < 1) // Don't report buckets as matched if any are locked
        {
            // -- peer has no buckets we want, don't send them again until something changes
            //    peer will still request buckets from this node if needed (< ncontent)
            vchDataOut.resize(8);
            memcpy(&vchDataOut[0], &now, 8);
            pfrom->PushMessage("smsgMatch", vchDataOut);
            if (fDebugSmsg)
                printf("Sending smsgMatch, %"PRI64d".\n", now);
        };
        
    } else
    if (strCommand == "smsgShow")
    {
        std::vector<unsigned char> vchData;
        vRecv >> vchData;
        
        if (vchData.size() < 4)
            return false;
        
        uint32_t nBuckets;
        memcpy(&nBuckets, &vchData[0], 4);
        
        if (vchData.size() < 4 + nBuckets * 8)
            return false;
        
        if (fDebugSmsg)
            printf("smsgShow: peer wants to see content of %u buckets.\n", nBuckets);
        
        std::map<int64_t, SecMsgBucket>::iterator itb;
        std::set<SecMsgToken>::iterator it;
        
        std::vector<unsigned char> vchDataOut;
        int64_t time;
        unsigned char* pIn = &vchData[4];
        for (uint32_t i = 0; i < nBuckets; ++i, pIn += 8)
        {
            memcpy(&time, pIn, 8);
            
            itb = smsgSets.find(time);
            if (itb == smsgSets.end())
            {
                if (fDebugSmsg)
                    printf("Don't have bucket %"PRI64d".\n", time);
                continue;
            };
            
            std::set<SecMsgToken>& tokenSet = (*itb).second.setTokens;
            
            vchDataOut.resize(8 + 16 * tokenSet.size());
            memcpy(&vchDataOut[0], &time, 8);
            
            unsigned char* p = &vchDataOut[8];
            for (it = tokenSet.begin(); it != tokenSet.end(); ++it)
            {
                memcpy(p, &it->timestamp, 8);
                memcpy(p+8, &it->sample, 8);
                
                p += 16;
            };
            pfrom->PushMessage("smsgHave", vchDataOut);
        };
        
        
    } else
    if (strCommand == "smsgHave")
    {
        // -- peer has these messages in bucket
        std::vector<unsigned char> vchData;
        vRecv >> vchData;
        
        if (vchData.size() < 8)
            return false;
        
        int n = (vchData.size() - 8) / 16;
        
        int64_t time;
        memcpy(&time, &vchData[0], 8);
        
        // -- Check time valid:
        int64_t now = GetTime();
        if (time < now - SMSG_RETENTION)
        {
            if (fDebugSmsg)
                printf("Not interested in peer bucket %"PRI64d", has expired.\n", time);
            return false;
        };
        if (time > now + SMSG_TIME_LEEWAY)
        {
            if (fDebugSmsg)
                printf("Not interested in peer bucket %"PRI64d", in the future.\n", time);
            pfrom->Misbehaving(1);
            return false;
        };
        
        if (smsgSets[time].nLockCount > 0)
        {
            if (fDebugSmsg)
                printf("Bucket %"PRI64d" lock count %u, waiting for message data from peer %u.\n", time, smsgSets[time].nLockCount, smsgSets[time].nLockPeerId);
            return false;
        }; 
        
        if (fDebugSmsg)
            printf("Sifting through bucket %"PRI64d".\n", time);
        
        std::vector<unsigned char> vchDataOut;
        vchDataOut.resize(8);
        memcpy(&vchDataOut[0], &vchData[0], 8);
        
        std::set<SecMsgToken>& tokenSet = smsgSets[time].setTokens;
        std::set<SecMsgToken>::iterator it;
        SecMsgToken token;
        unsigned char* p = &vchData[8];
        
        for (int i = 0; i < n; ++i)
        {
            memcpy(&token.timestamp, p, 8);
            memcpy(&token.sample, p+8, 8);
            
            it = tokenSet.find(token);
            if (it == tokenSet.end())
            {
                int nd = vchDataOut.size();
                vchDataOut.resize(nd + 16);
                memcpy(&vchDataOut[nd], p, 16);
            };
            
            p += 16;
        };
        
        if (vchDataOut.size() > 8)
        {
            if (fDebugSmsg)
            {
                printf("Asking peer for  %"PRIszu" messages.\n", (vchDataOut.size() - 8) / 16);
                printf("Locking bucket %"PRIszu" for peer %u.\n", time, pfrom->smsgData.nPeerId);
            };
            smsgSets[time].nLockCount   = 3; // lock this bucket for at most 3 * SMSG_THREAD_DELAY seconds, unset when peer sends smsgMsg
            smsgSets[time].nLockPeerId  = pfrom->smsgData.nPeerId;
            pfrom->PushMessage("smsgWant", vchDataOut);
        };
    } else
    if (strCommand == "smsgWant")
    {
        std::vector<unsigned char> vchData;
        vRecv >> vchData;
        
        if (vchData.size() < 8)
            return false;
        
        std::vector<unsigned char> vchOne;
        std::vector<unsigned char> vchBunch;
        
        vchBunch.resize(4+8); // nmessages + bucketTime
        
        int n = (vchData.size() - 8) / 16;
        
        int64_t time;
        uint32_t nBunch = 0;
        memcpy(&time, &vchData[0], 8);
        
        std::map<int64_t, SecMsgBucket>::iterator itb;
        itb = smsgSets.find(time);
        if (itb == smsgSets.end())
        {
            if (fDebugSmsg)
                printf("Don't have bucket %"PRI64d".\n", time);
            return false;
        };
        
        std::set<SecMsgToken>& tokenSet = itb->second.setTokens;
        std::set<SecMsgToken>::iterator it;
        SecMsgToken token;
        unsigned char* p = &vchData[8];
        for (int i = 0; i < n; ++i)
        {
            memcpy(&token.timestamp, p, 8);
            memcpy(&token.sample, p+8, 8);
            
            it = tokenSet.find(token);
            if (it == tokenSet.end())
            {
                if (fDebugSmsg)
                    printf("Don't have wanted message %"PRI64d".\n", token.timestamp);
            } else
            {
                //printf("Have message at %"PRI64d".\n", it->offset);
                token.offset = it->offset;
                //printf("winb before SecureMsgRetrieve %"PRI64d".\n", token.timestamp);
                
                // -- place in vchOne so if SecureMsgRetrieve fails it won't corrupt vchBunch
                if (SecureMsgRetrieve(token, vchOne) == 0)
                {
                    nBunch++;
                    vchBunch.insert(vchBunch.end(), vchOne.begin(), vchOne.end()); // append
                } else
                {
                    printf("SecureMsgRetrieve failed %"PRI64d".\n", token.timestamp);
                };
                
                if (nBunch >= 500
                    || vchBunch.size() >= 96000)
                {
                    if (fDebugSmsg)
                        printf("Break bunch %u, %"PRIszu".\n", nBunch, vchBunch.size());
                    break; // end here, peer will send more want messages if needed.
                };
            };
            p += 16;
        };
        
        if (nBunch > 0)
        {
            if (fDebugSmsg)
                printf("Sending block of %u messages for bucket %"PRI64d".\n", nBunch, time);
            
            memcpy(&vchBunch[0], &nBunch, 4);
            memcpy(&vchBunch[4], &time, 8);
            pfrom->PushMessage("smsgMsg", vchBunch);
        };
    } else
    if (strCommand == "smsgMsg")
    {
        std::vector<unsigned char> vchData;
        vRecv >> vchData;
        
        if (fDebugSmsg)
            printf("smsgMsg vchData.size() %"PRIszu".\n", vchData.size());
        
        SecureMsgReceive(pfrom, vchData);
    } else
    if (strCommand == "smsgMatch")
    {
        std::vector<unsigned char> vchData;
        vRecv >> vchData;
        
        
        if (vchData.size() < 8)
        {
            printf("smsgMatch, not enough data %"PRIszu".\n", vchData.size());
            pfrom->Misbehaving(1);
            return false;
        };
        
        int64_t time;
        memcpy(&time, &vchData[0], 8);
        
        int64_t now = GetTime();
        if (time > now + SMSG_TIME_LEEWAY)
        {
            printf("Warning: Peer buckets matched in the future: %"PRI64d".\nEither this node or the peer node has the incorrect time set.\n", time);
            if (fDebugSmsg)
                printf("Peer match time set to now.\n");
            time = now;
        };
        
        pfrom->smsgData.lastMatched = time;
        
        if (fDebugSmsg)
            printf("Peer buckets matched at %"PRI64d".\n", time);
        
    } else
    if (strCommand == "smsgPing")
    {
        // -- smsgPing is the initial message, send reply
        pfrom->PushMessage("smsgPong");
    } else
    if (strCommand == "smsgPong")
    {
        if (fDebugSmsg)
             printf("Peer replied, secure messaging enabled.\n");
        
        pfrom->smsgData.fEnabled = true;
    } else
    if (strCommand == "smsgDisabled")
    {
        // -- peer has disabled secure messaging.
        
        pfrom->smsgData.fEnabled = false;
        
        if (fDebugSmsg)
            printf("Peer %u has disabled secure messaging.\n", pfrom->smsgData.nPeerId);
        
    } else
    if (strCommand == "smsgIgnore")
    {
        // -- peer is reporting that it will ignore this node until time.
        //    Ignore peer too
        std::vector<unsigned char> vchData;
        vRecv >> vchData;
        
        if (vchData.size() < 8)
        {
            printf("smsgIgnore, not enough data %"PRIszu".\n", vchData.size());
            pfrom->Misbehaving(1);
            return false;
        };
        
        int64_t time;
        memcpy(&time, &vchData[0], 8);
        
        pfrom->smsgData.ignoreUntil = time;
        
        if (fDebugSmsg)
            printf("Peer %u is ignoring this node until %"PRI64d", ignore peer too.\n", pfrom->smsgData.nPeerId, time);
    } else
    {
        // Unknown message
    };
    
    }; //  LOCK(cs_smsg);
    
    return true;
};

bool SecureMsgSendData(CNode* pto, bool fSendTrickle)
{
    /*
        Called from ProcessMessage
        Runs in ThreadMessageHandler2
    */
    
    //printf("SecureMsgSendData() %s.\n", pto->addrName.c_str());
    
    
    int64_t now = GetTime();
    
    if (pto->smsgData.lastSeen == 0)
    {
        // -- first contact
        pto->smsgData.nPeerId = nPeerIdCounter++;
        if (fDebugSmsg)
            printf("SecureMsgSendData() new node %s, peer id %u.\n", pto->addrName.c_str(), pto->smsgData.nPeerId);
        // -- Send smsgPing once, do nothing until receive 1st smsgPong (then set fEnabled)
        pto->PushMessage("smsgPing");
        pto->smsgData.lastSeen = GetTime();
        return true;
    } else
    if (!pto->smsgData.fEnabled
        || now - pto->smsgData.lastSeen < SMSG_SEND_DELAY
        || now < pto->smsgData.ignoreUntil)
    {
        return true;
    };
    
    // -- When nWakeCounter == 0, resend bucket inventory.  
    if (pto->smsgData.nWakeCounter < 1)
    {
        pto->smsgData.lastMatched = 0;
        pto->smsgData.nWakeCounter = 3 + GetRandInt(90);  // set to a random time between [3, 120] * 10 (SMSG_SEND_DELAY) seconds
        
        if (fDebugSmsg)
            printf("SecureMsgSendData(): nWakeCounter expired, sending bucket inventory to %s.\n"
            "Now %"PRI64d" next wake counter %u\n", pto->addrName.c_str(), now, pto->smsgData.nWakeCounter);
    };
    pto->smsgData.nWakeCounter--;
    
    {
        LOCK(cs_smsg);
        std::map<int64_t, SecMsgBucket>::iterator it;
        
        uint32_t nBuckets = smsgSets.size();
        if (nBuckets > 0) // no need to send keep alive pkts, coin messages already do that
        {
            std::vector<unsigned char> vchData;
            // should reserve?
            vchData.reserve(4 + nBuckets*16); // timestamp + size + hash
            
            uint32_t nBucketsShown = 0;
            vchData.resize(4);
            
            unsigned char* p = &vchData[4];
            for (it = smsgSets.begin(); it != smsgSets.end(); ++it)
            {
                SecMsgBucket &bkt = it->second;
                
                uint32_t nMessages = bkt.setTokens.size();
                
                if (bkt.timeChanged < pto->smsgData.lastMatched     // peer has this bucket
                    || nMessages < 1)                               // this bucket is empty
                    continue; 
                
                
                uint32_t hash = bkt.hash;
                
                vchData.resize(vchData.size() + 16);
                memcpy(p, &it->first, 8);
                memcpy(p+8, &nMessages, 4);
                memcpy(p+12, &hash, 4);
                
                p += 16;
                nBucketsShown++;
                //if (fDebug)
                //    printf("Sending bucket %"PRI64d", size %d \n", it->first, it->second.size());
            };
            
            if (vchData.size() > 4)
            {
                memcpy(&vchData[0], &nBucketsShown, 4);
                if (fDebugSmsg)
                    printf("Sending %d bucket headers.\n", nBucketsShown);
                
                pto->PushMessage("smsgInv", vchData);
            };
        };
    }
    
    pto->smsgData.lastSeen = GetTime();
    
    return true;
};


static int SecureMsgInsertAddress(CKeyID& hashKey, CPubKey& pubKey, CSmesgPubKeyDB& addrpkdb)
{
    /* insert key hash and public key to addressdb
        
        should have LOCK(cs_smsg) where db is opened
        
        returns
            0 success
            4 address is already in db
            5 error
    */
    
    
    if (addrpkdb.ExistsPK(hashKey))
    {
        //printf("DB already contains public key for address.\n");
        CPubKey cpkCheck;
        if (!addrpkdb.ReadPK(hashKey, cpkCheck))
        {
            printf("addrpkdb.Read failed.\n");
        } else
        {
            if (cpkCheck != pubKey)
                printf("DB already contains existing public key that does not match .\n");
        };
        return 4;
    };
    
    if (!addrpkdb.WritePK(hashKey, pubKey))
    {
        printf("Write pair failed.\n");
        return 5;
    };
    
    return 0;
};

int SecureMsgInsertAddress(CKeyID& hashKey, CPubKey& pubKey)
{
    int rv;
    {
        LOCK(cs_smsg);
        CSmesgPubKeyDB addrpkdb("cr+");
        
        rv = SecureMsgInsertAddress(hashKey, pubKey, addrpkdb);
    }
    return rv;
};


static bool ScanBlock(CBlock& block, CTxDB& txdb, CSmesgPubKeyDB& addrpkdb,
    uint32_t& nTransactions, uint32_t& nInputs, uint32_t& nPubkeys, uint32_t& nDuplicates)
{
    // -- should have LOCK(cs_smsg) where db is opened
    BOOST_FOREACH(CTransaction& tx, block.vtx)
    {
        if (!tx.IsStandard())
            continue; // leave out coinbase and others
        
        /*
        Look at the inputs of every tx.
        If the inputs are standard, get the pubkey from scriptsig and
        look for the corresponding output (the input(output of other tx) to the input of this tx)
        get the address from scriptPubKey
        add to db if address is unique.
        
        Would make more sense to do this the other way around, get address first for early out.
        
        */
        
        for (unsigned int i = 0; i < tx.vin.size(); i++)
        {
            CScript *script = &tx.vin[i].scriptSig;
            
            opcodetype opcode;
            valtype vch;
            CScript::const_iterator pc = script->begin();
            CScript::const_iterator pend = script->end();
            
            uint256 prevoutHash;
            CKey key;
            
            // -- matching address is in scriptPubKey of previous tx output
            while (pc < pend)
            {
                if (!script->GetOp(pc, opcode, vch))
                    break;
                // -- opcode is the length of the following data, compressed public key is always 33
                if (opcode == 33)
                {
                    key.SetPubKey(vch);
                    
                    key.SetCompressedPubKey(); // ensure key is compressed
                    CPubKey pubKey = key.GetPubKey();
                    
                    if (!pubKey.IsValid()
                        || !pubKey.IsCompressed())
                    {
                        printf("Public key is invalid %s.\n", ValueString(pubKey.Raw()).c_str());
                        continue;
                    };
                    
                    prevoutHash = tx.vin[i].prevout.hash;
                    CTransaction txOfPrevOutput;
                    if (!txdb.ReadDiskTx(prevoutHash, txOfPrevOutput))
                    {
                        printf("Could not get transaction for hash: %s.\n", prevoutHash.ToString().c_str());
                        continue;
                    };
                    
                    unsigned int nOut = tx.vin[i].prevout.n;
                    if (nOut >= txOfPrevOutput.vout.size())
                    {
                        printf("Output %u, not in transaction: %s.\n", nOut, prevoutHash.ToString().c_str());
                        continue;
                    };
                    
                    CTxOut *txOut = &txOfPrevOutput.vout[nOut];
                    
                    CTxDestination addressRet;
                    if (!ExtractDestination(txOut->scriptPubKey, addressRet))
                    {
                        printf("ExtractDestination failed: %s.\n", prevoutHash.ToString().c_str());
                        break;
                    };
                    
                    
                    CBitcoinAddress coinAddress(addressRet);
                    CKeyID hashKey;
                    if (!coinAddress.GetKeyID(hashKey))
                    {
                        printf("coinAddress.GetKeyID failed: %s.\n", coinAddress.ToString().c_str());
                        break;
                    };
                    
                    int rv = SecureMsgInsertAddress(hashKey, pubKey, addrpkdb);
                    if (rv != 0)
                    {
                        if (rv == 4)
                            nDuplicates++;
                        break;
                    };
                    nPubkeys++;
                    break;
                };
                
                //printf("opcode %d, %s, value %s.\n", opcode, GetOpName(opcode), ValueString(vch).c_str());
            };
            nInputs++;
        };
        nTransactions++;
        
        if (nTransactions % 10000 == 0) // for ScanChainForPublicKeys
        {
            printf("Scanning transaction no. %u.\n", nTransactions);
        };
    };
    return true;
};


bool SecureMsgScanBlock(CBlock& block)
{
    /*
    scan block for public key addresses
    called from ProcessMessage() in main where strCommand == "block"
    */
    
    if (fDebugSmsg)
        printf("SecureMsgScanBlock().\n");
    
    uint32_t nTransactions  = 0;
    uint32_t nInputs        = 0;
    uint32_t nPubkeys       = 0;
    uint32_t nDuplicates    = 0;
    
    {
        LOCK(cs_smsg);
        
        CSmesgPubKeyDB addrpkdb("cw");
        CTxDB txdb("r");
        
        
        ScanBlock(block, txdb, addrpkdb,
            nTransactions, nInputs, nPubkeys, nDuplicates);
    }
    
    if (fDebugSmsg)
        printf("Found %u transactions, %u inputs, %u new public keys, %u duplicates.\n", nTransactions, nInputs, nPubkeys, nDuplicates);
    
    return true;
};

bool ScanChainForPublicKeys(CBlockIndex* pindexStart)
{
    printf("Scanning block chain for public keys.\n");
    int64_t nStart = GetTimeMillis();
    
    if (fDebugSmsg)
        printf("From height %u.\n", pindexStart->nHeight);
    
    // -- public keys are in txin.scriptSig
    //    matching addresses are in scriptPubKey of txin's referenced output
    
    uint32_t nBlocks        = 0;
    uint32_t nTransactions  = 0;
    uint32_t nInputs        = 0;
    uint32_t nPubkeys       = 0;
    uint32_t nDuplicates    = 0;
    
    {
        LOCK(cs_smsg);
    
        CSmesgPubKeyDB addrpkdb("cw");
        CTxDB txdb("r");
        
        CBlockIndex* pindex = pindexStart;
        while (pindex)
        {
            nBlocks++;
            CBlock block;
            block.ReadFromDisk(pindex, true);
            
            ScanBlock(block, txdb, addrpkdb,
                nTransactions, nInputs, nPubkeys, nDuplicates);
            
            pindex = pindex->pnext;
        };
    };
    //addrpkdb.Close(); // necessary?
    
    printf("Scanned %u blocks, %u transactions, %u inputs\n", nBlocks, nTransactions, nInputs);
    printf("Found %u public keys, %u duplicates.\n", nPubkeys, nDuplicates);
    printf("Took %lld ms\n", GetTimeMillis() - nStart);
    
    return true;
};

bool SecureMsgScanBlockChain()
{
    TRY_LOCK(cs_main, lockMain);
    if (lockMain)
    {
        CBlockIndex *pindexScan = pindexGenesisBlock;
        if (pindexScan == NULL)
        {
            printf("Error: pindexGenesisBlock not set.\n");
            return false;
        };
        
        
        try
        { // -- in try to catch errors opening db, 
            if (!ScanChainForPublicKeys(pindexScan))
                return false;
        } catch (std::exception& e)
        {
            printf("ScanChainForPublicKeys() threw: %s.\n", e.what());
            return false;
        };
    } else
    {
        printf("ScanChainForPublicKeys() Could not lock main.\n");
        return false;
    };
    
    return true;
};

int SecureMsgScanMessage(unsigned char *pHeader, unsigned char *pPayload, uint32_t nPayload)
{
    /* 
    Check if message belongs to this node.
    If so add to inbox db.
    
    returns
        0 success,
        1 error
        2 no match
        
    */
    
    if (fDebugSmsg)
        printf("SecureMsgScanMessage()\n");
    
    std::string addressTo;
    MessageData msg; // placeholder
    bool fOwnMessage = false;
    
    // TODO: whitelist of addresses to receive on
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, std::string)& entry, pwalletMain->mapAddressBook)
    {
        if (!IsMine(*pwalletMain, entry.first))
            continue;
        
        CBitcoinAddress coinAddress(entry.first);
        addressTo = coinAddress.ToString();
        
        if (SecureMsgDecrypt(true, addressTo, pHeader, pPayload, nPayload, msg) == 0)
        {
            if (fDebugSmsg)
                printf("Decrypted message with %s.\n", addressTo.c_str());
            fOwnMessage = true;
            break;
        };
    };
    
    if (fOwnMessage)
    {
        // -- save to inbox
        {
            LOCK(cs_smsgInbox);
            
            CSmesgInboxDB dbInbox("cw");
            
            std::vector<unsigned char> vchKey;
            vchKey.resize(16); // timestamp8 + sample8
            memcpy(&vchKey[0], pHeader + 5, 8); // timestamp
            memcpy(&vchKey[8], pPayload, 8);    // sample
            
            SecInboxMsg smsgInbox;
            smsgInbox.timeReceived  = GetTime();
            smsgInbox.sAddrTo       = addressTo;
            
            // -- data may not be contiguous
            smsgInbox.vchMessage.resize(SMSG_HDR_LEN + nPayload);
            memcpy(&smsgInbox.vchMessage[0], pHeader, SMSG_HDR_LEN);
            memcpy(&smsgInbox.vchMessage[SMSG_HDR_LEN], pPayload, nPayload);
            
            if (dbInbox.ExistsSmesg(vchKey))
            {
                if (fDebugSmsg)
                    printf("Message already exists in inbox db.\n");
            } else
            {
                dbInbox.WriteSmesg(vchKey, smsgInbox);
                
                // -- Add to unread list, must be a better way...
                std::vector<unsigned char> vchUnread;
                dbInbox.ReadUnread(vchUnread);
                vchUnread.insert(vchUnread.end(), vchKey.begin(), vchKey.end()); // append
                dbInbox.WriteUnread(vchUnread);
                
                NotifySecMsgInboxChanged(smsgInbox);
                printf("SecureMsg saved to inbox, received with %s.\n", addressTo.c_str());
            };
        }
    };
    
    return 0;
};

int SecureMsgGetLocalKey(CKeyID& ckid, CPubKey& cpkOut)
{
    CKey key;
    if (!pwalletMain->GetKey(ckid, key))
        return 4;
    
    key.SetCompressedPubKey(); // make sure key is compressed
    
    cpkOut = key.GetPubKey();
    if (!cpkOut.IsValid()
        || !cpkOut.IsCompressed())
    {
        printf("Public key is invalid %s.\n", ValueString(cpkOut.Raw()).c_str());
        return 1;
    };
    
    return 0;
};

int SecureMsgGetLocalPublicKey(std::string& strAddress, std::string& strPublicKey)
{
    /* returns
        0 success,
        1 error
        2 invalid address
        3 address does not refer to a key
        4 address not in wallet
    */
    
    CBitcoinAddress address;
    if (!address.SetString(strAddress))
        return 2; // Invalid CinniCoin address
    
    CKeyID keyID;
    if (!address.GetKeyID(keyID))
        return 3;
    
    int rv;
    CPubKey pubKey;
    if ((rv = SecureMsgGetLocalKey(keyID, pubKey)) != 0)
        return rv;
    
    strPublicKey = EncodeBase58(pubKey.Raw());
    
    return 0;
};

int SecureMsgGetStoredKey(CKeyID& ckid, CPubKey& cpkOut)
{
    /* returns
        0 success,
        1 error
        2 public key not in database
    */
    if (fDebugSmsg)
        printf("SecureMsgGetStoredKey().\n");
    
    CSmesgPubKeyDB addrpkdb("r");
    
    if (!addrpkdb.ReadPK(ckid, cpkOut))
    {
        //printf("addrpkdb.Read failed: %s.\n", coinAddress.ToString().c_str());
        return 2;
    };
    
    addrpkdb.Close(); // necessary?
    
    return 0;
};

int SecureMsgAddAddress(std::string& address, std::string& publicKey)
{
    /*
        Add address and matching public key to the database
        address and publicKey are in base58
        
        returns
            0 success
            1 address is invalid
            2 publicKey is invalid
            3 publicKey != address
            4 address is already in db
            5 error
    */
    
    CBitcoinAddress coinAddress(address);
    
    if (!coinAddress.IsValid())
    {
        printf("Address is not valid: %s.\n", address.c_str());
        return 1;
    };
    
    CKeyID hashKey;
    
    if (!coinAddress.GetKeyID(hashKey))
    {
        printf("coinAddress.GetKeyID failed: %s.\n", coinAddress.ToString().c_str());
        return 1;
    };
    
    std::vector<unsigned char> vchTest;
    DecodeBase58(publicKey, vchTest);
    CPubKey pubKey(vchTest);
    
    // -- check that public key matches address hash
    CKey keyT;
    if (!keyT.SetPubKey(pubKey))
    {
        printf("SetPubKey failed.\n");
        return 2;
    };
    
    keyT.SetCompressedPubKey();
    CPubKey pubKeyT = keyT.GetPubKey();
    
    CBitcoinAddress addressT(address);
    
    if (addressT.ToString().compare(address) != 0)
    {
        printf("Public key does not hash to address, addressT %s.\n", addressT.ToString().c_str());
        return 3;
    };
    
    return SecureMsgInsertAddress(hashKey, pubKey);
};

int SecureMsgRetrieve(SecMsgToken &token, std::vector<unsigned char>& vchData)
{
    if (fDebugSmsg)
        printf("SecureMsgRetrieve() %"PRI64d".\n", token.timestamp);
    
    // -- has cs_smsg lock from SecureMsgReceiveData
    
    fs::path pathSmsgDir = GetDataDir() / "smsgStore";
    
    
    int64_t bucket = token.timestamp - (token.timestamp % SMSG_BUCKET_LEN);
    std::string fileName = boost::lexical_cast<std::string>(bucket) + "_01.dat";
    //printf("bucket %"PRI64d".\n", bucket);
    //printf("bucket lld %lld.\n", bucket);
    //printf("fileName %s.\n", fileName.c_str());
    
    fs::path fullpath = pathSmsgDir / fileName;
    
    FILE *fp;
    errno = 0;
    if (!(fp = fopen(fullpath.string().c_str(), "rb")))
    {
        printf("Error opening file: %s\nPath %s\n", strerror(errno), fullpath.string().c_str());
        return 1;
    };
    
    errno = 0;
    if (fseek(fp, token.offset, SEEK_SET) != 0)
    {
        printf("fseek, strerror: %s.\n", strerror(errno));
        fclose(fp);
        return 1;
    };
    
    SecureMessage smsg;
    errno = 0;
    if (fread(&smsg.hash[0], sizeof(unsigned char), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN)
    {
        printf("fread header failed: %s\n", strerror(errno));
        fclose(fp);
        return 1;
    };
    
    vchData.resize(SMSG_HDR_LEN + smsg.nPayload);
    
    memcpy(&vchData[0], &smsg.hash[0], SMSG_HDR_LEN);
    errno = 0;
    if (fread(&vchData[SMSG_HDR_LEN], sizeof(unsigned char), smsg.nPayload, fp) != smsg.nPayload)
    {
        printf("fread data failed: %s. Wanted %u bytes.\n", strerror(errno), smsg.nPayload);
        fclose(fp);
        return 1;
    };
    
    
    fclose(fp);
    
    return 0;
};

int SecureMsgReceive(CNode* pfrom, std::vector<unsigned char>& vchData)
{
    if (fDebugSmsg)
        printf("SecureMsgReceive().\n");
    
    if (vchData.size() < 12) // nBunch4 + timestamp8
    {
        printf("Error: not enough data.\n");
        return 1;
    };
    
    uint32_t nBunch;
    int64_t bktTime;
    
    memcpy(&nBunch, &vchData[0], 4);
    memcpy(&bktTime, &vchData[4], 8);
    
    
    // -- check bktTime ()
    //    bucket may not exist yet - will be created when messages are added
    int64_t now = GetTime();
    if (bktTime > now + SMSG_TIME_LEEWAY)
    {
        if (fDebugSmsg)
            printf("bktTime > now.\n");
        // misbehave?
        return 1;
    } else
    if (bktTime < now - SMSG_RETENTION)
    {
        if (fDebugSmsg)
            printf("bktTime < now - SMSG_RETENTION.\n");
        // misbehave?
        return 1;
    };
    
    std::map<int64_t, SecMsgBucket>::iterator itb;
    
    if (nBunch == 0 || nBunch > 500)
    {
        printf("Error: Invalid no. messages received in bunch %u, for bucket %"PRI64d".\n", nBunch, bktTime);
        pfrom->Misbehaving(1);
        
        // -- release lock on bucket if it exists
        itb = smsgSets.find(bktTime);
        if (itb != smsgSets.end())
            itb->second.nLockCount = 0;
        return 1;
    };
    
    uint32_t n = 12;
    
    for (uint32_t i = 0; i < nBunch; ++i)
    {
        if (vchData.size() - n < SMSG_HDR_LEN)
        {
            printf("Error: not enough data, n = %u.\n", n);
            break;
        };
        
        SecureMessage* psmsg = (SecureMessage*) &vchData[n];
        
        int rv;
        if ((rv = SecureMsgValidate(&vchData[n], &vchData[n + SMSG_HDR_LEN], psmsg->nPayload)) != 0)
        {
            // message dropped
            if (rv == 2) // invalid proof of work
            {
                pfrom->Misbehaving(10);
            } else
            {
                pfrom->Misbehaving(1);
            };
            continue;
        };
        
        // -- store message, but don't hash bucket
        if (SecureMsgStore(&vchData[n], &vchData[n + SMSG_HDR_LEN], psmsg->nPayload, false) != 0)
        {
            // message dropped
            break; // continue?
        };
        
        
        if (SecureMsgScanMessage(&vchData[n], &vchData[n + SMSG_HDR_LEN], psmsg->nPayload) != 0)
        {
            // message recipient is not this node (or failed)
        };
        
        n += SMSG_HDR_LEN + psmsg->nPayload;
    };
    
    // -- if messages have been added, bucket must exist now
    itb = smsgSets.find(bktTime);
    if (itb == smsgSets.end())
    {
        if (fDebugSmsg)
            printf("Don't have bucket %"PRI64d".\n", bktTime);
        return 1;
    };
    
    itb->second.nLockCount  = 0; // this node has received data from peer, release lock
    itb->second.nLockPeerId = 0;
    itb->second.hashBucket();
    
    return 0;
};

int SecureMsgStore(unsigned char *pHeader, unsigned char *pPayload, uint32_t nPayload, bool fUpdateBucket)
{
    if (fDebugSmsg)
        printf("SecureMsgStore()\n");
    
    if (!pHeader
        || !pPayload)
    {
        printf("Error: null pointer to header or payload.\n");
        return 1;
    };
    
    SecureMessage* psmsg = (SecureMessage*) pHeader;
    
    
    long int ofs;
    
    fs::path pathSmsgDir = GetDataDir() / "smsgStore";
    fs::create_directory(pathSmsgDir);
    
    int64_t now = GetTime();
    if (psmsg->timestamp > now + SMSG_TIME_LEEWAY)
    {
        printf("Message > now.\n");
        return 1;
    } else
    if (psmsg->timestamp < now - SMSG_RETENTION)
    {
        printf("Message < SMSG_RETENTION.\n");
        return 1;
    };
    
    int64_t bucket = psmsg->timestamp - (psmsg->timestamp % SMSG_BUCKET_LEN);
    std::string fileName = boost::lexical_cast<std::string>(bucket) + "_01.dat";
    
    fs::path fullpath = pathSmsgDir / fileName;
    
    {
        // -- must lock cs_smsg before calling
        //LOCK(cs_smsg);
        
        SecMsgToken token(psmsg->timestamp, pPayload, nPayload, 0);
        
        std::set<SecMsgToken>::iterator it;
        it = smsgSets[bucket].setTokens.find(token);
        if (it != smsgSets[bucket].setTokens.end())
        {
            printf("Already have message.\n");
            if (fDebugSmsg)
            {
                int64_t time;
                printf("ts: %"PRI64d" sample ", token.timestamp);
                for (int i = 0; i < 8;++i)
                    printf("%c.\n", token.sample[i]);
                printf("\n");
            };
            return 1;
        };
        
        FILE *fp;
        errno = 0;
        if (!(fp = fopen(fullpath.string().c_str(), "ab")))
        {
            printf("Error opening file: %s\n", strerror(errno));
            return 1;
        };
        
        
        ofs = ftell(fp);
        
        if (fwrite(pHeader, sizeof(unsigned char), SMSG_HDR_LEN, fp) != (size_t)SMSG_HDR_LEN
            || fwrite(pPayload, sizeof(unsigned char), nPayload, fp) != nPayload)
        {
            printf("fwrite failed: %s\n", strerror(errno));
            fclose(fp);
            return 1;
        };
        
        token.offset = ofs;
        
        fclose(fp);
        
        smsgSets[bucket].setTokens.insert(token);
        
        if (fUpdateBucket)
            smsgSets[bucket].hashBucket();
    };
    
    //if (fDebugSmsg)
    printf("SecureMsg added to bucket %"PRI64d".\n", bucket);
    return 0;
};

int SecureMsgStore(SecureMessage& smsg, bool fUpdateBucket)
{
    return SecureMsgStore(&smsg.hash[0], smsg.pPayload, smsg.nPayload, fUpdateBucket);
};
  
int SecureMsgValidate(unsigned char *pHeader, unsigned char *pPayload, uint32_t nPayload)
{
    /*
    returns
        0 success
        1 error
        2 invalid hash
        3 checksum mismatch
        4 invalid version
        5 payload is too large
    */
    SecureMessage* psmsg = (SecureMessage*) pHeader;
    
    if (psmsg->version != 1)
        return 4;
    
    if (nPayload > SMSG_MAX_MSG_WORST)
        return 5;
    
    unsigned char civ[32];
    unsigned char sha256Hash[32];
    int rv = 2; // invalid
    
    uint32_t nonse;
    memcpy(&nonse, &psmsg->nonse[0], 4);
    
    if (fDebugSmsg)
        printf("SecureMsgValidate() nonse %u.\n", nonse);
    
    for (int i = 0; i < 32; i+=4)
        memcpy(civ+i, &nonse, 4);
    
    HMAC_CTX ctx;
    HMAC_CTX_init(&ctx);
    
    unsigned int nBytes;
    if (!HMAC_Init_ex(&ctx, &civ[0], 32, EVP_sha256(), NULL)
        || !HMAC_Update(&ctx, (unsigned char*) pHeader+4, SMSG_HDR_LEN-4)
        || !HMAC_Update(&ctx, (unsigned char*) pPayload, nPayload)
        || !HMAC_Update(&ctx, pPayload, nPayload)
        || !HMAC_Final(&ctx, sha256Hash, &nBytes)
        || nBytes != 32)
    {
        if (fDebugSmsg)
            printf("HMAC error.\n");
        rv = 1; // error
    } else
    {
        if (sha256Hash[31] == 0
            && sha256Hash[30] == 0
            && (~(sha256Hash[29]) & ((1<<0) || (1<<1) || (1<<2)) ))
        {
            if (fDebugSmsg)
                printf("Hash Valid.\n");
            rv = 0; // smsg is valid
        };
        
        if (memcmp(psmsg->hash, sha256Hash, 4) != 0)
        {
             if (fDebugSmsg)
                printf("Checksum mismatch.\n");
            rv = 3; // checksum mismatch
        }
    }
    HMAC_CTX_cleanup(&ctx);
    
    return rv;
};

int SecureMsgSetHash(unsigned char *pHeader, unsigned char *pPayload, uint32_t nPayload)
{
    /*  proof of work and checksum
        
        May run in a thread, if shutdown detected, return.
        
        returns:
            0 success
            1 error
            2 stopped due to node shutdown
        
    */
    
    SecureMessage* psmsg = (SecureMessage*) pHeader;
    
    int64_t nStart = GetTimeMillis();
    unsigned char civ[32];
    unsigned char sha256Hash[32];
    bool found = false;
    HMAC_CTX ctx;
    HMAC_CTX_init(&ctx);
    
    uint32_t nonse = 0;
    
    // -- break for HMAC_CTX_cleanup
    for (;;)
    {
        if (fShutdown)
           break;
        
        //psmsg->timestamp = GetTime();
        //memcpy(&psmsg->timestamp, &now, 8);
        memcpy(&psmsg->nonse[0], &nonse, 4);
        
        for (int i = 0; i < 32; i+=4)
            memcpy(civ+i, &nonse, 4);
        
        unsigned int nBytes;
        if (!HMAC_Init_ex(&ctx, &civ[0], 32, EVP_sha256(), NULL)
            || !HMAC_Update(&ctx, (unsigned char*) pHeader+4, SMSG_HDR_LEN-4)
            || !HMAC_Update(&ctx, (unsigned char*) pPayload, nPayload)
            || !HMAC_Update(&ctx, pPayload, nPayload)
            || !HMAC_Final(&ctx, sha256Hash, &nBytes)
            || nBytes != 32)
            break;
        
         
        if (sha256Hash[31] == 0
            && sha256Hash[30] == 0
            && (~(sha256Hash[29]) & ((1<<0) || (1<<1) || (1<<2)) ))
        //    && sha256Hash[29] == 0)
        {
            found = true;
            //if (fDebugSmsg)
            //    printf("Match %u\n", nonse);
            break;
        }
        //if (nonse >= UINT32_MAX)
        if (nonse >= 4294967295U)
        {
            if (fDebugSmsg)
                printf("No match %u\n", nonse);
            break;
            //return 1; 
        }    
        nonse++;
    };
    
    HMAC_CTX_cleanup(&ctx);
    
    if (fShutdown)
    {
        if (fDebugSmsg)
            printf("SecureMsgSetHash() stopped, shutdown detected.\n");
        return 2;
    };
    
    if (!found)
    {
        if (fDebugSmsg)
            printf("SecureMsgSetHash() failed, took %lld ms, nonse %u\n", GetTimeMillis() - nStart, nonse);
        return 1;
    };
    
    memcpy(psmsg->hash, sha256Hash, 4);
    
    if (fDebugSmsg)
        printf("SecureMsgSetHash() took %lld ms, nonse %u\n", GetTimeMillis() - nStart, nonse);
    
    return 0;
};

int SecureMsgEncrypt(SecureMessage& smsg, std::string& addressFrom, std::string& addressTo, std::string& message)
{
    /* Create a secure message
        
        Using similar method to bitmessage.
        If bitmessage is secure this should be too.
        https://bitmessage.org/wiki/Encryption
        
        Some differences:
        bitmessage seems to use curve sect283r1
        Cinnicoin addresses use secp256k1
        
        returns
            2       message is too long.
            3       addressFrom is invalid.
            4       addressTo is invalid.
            5       Could not get public key for addressTo.
            6       ECDH_compute_key failed
            7       Could not get private key for addressFrom.
            8       Could not allocate memory.
            9       Could not compress message data.
            10      Could not generate MAC.
            11      Encrypt failed.
    */
    
    if (fDebugSmsg)
        printf("SecureMsgEncrypt(%s, %s, ...)\n", addressFrom.c_str(), addressTo.c_str());
    
    
    if (message.size() > SMSG_MAX_MSG_BYTES)
    {
        printf("Message is too long, %"PRIszu".\n", message.size());
        return 2;
    };
    
    smsg.version = 1;
    smsg.timestamp = GetTime();
    memset(smsg.destHash, 0, 20); // Not used yet
    
    
    bool fSendAnonymous;
    CBitcoinAddress coinAddrFrom;
    CKeyID ckidFrom;
    CKey keyFrom;
    
    if (addressFrom.compare("anon") == 0)
    {
        fSendAnonymous = true;
        
    } else
    {
        fSendAnonymous = false;
        
        if (!coinAddrFrom.SetString(addressFrom))
        {
            printf("addressFrom is not valid.\n");
            return 3;
        };
        
        if (!coinAddrFrom.GetKeyID(ckidFrom))
        {
            printf("coinAddrFrom.GetKeyID failed: %s.\n", coinAddrFrom.ToString().c_str());
            return 3;
        };
    };
    
    
    CBitcoinAddress coinAddrDest;
    CKeyID ckidDest;
    
    if (!coinAddrDest.SetString(addressTo))
    {
        printf("addressTo is not valid.\n");
        return 4;
    };
    
    if (!coinAddrDest.GetKeyID(ckidDest))
    {
        printf("coinAddrDest.GetKeyID failed: %s.\n", coinAddrDest.ToString().c_str());
        return 4;
    };
    
    // -- public key K is the destination address
    CPubKey cpkDestK;
    if (SecureMsgGetStoredKey(ckidDest, cpkDestK) != 0)
    {
        // -- maybe it's a local key (outbox?)
        if (SecureMsgGetLocalKey(ckidDest, cpkDestK) != 0)
        {
            printf("Could not get public key for destination address.\n");
            return 5;
        };
    };
    
    
    // -- Generate 16 random bytes as IV.
    RandAddSeedPerfmon();
    RAND_bytes(&smsg.iv[0], 16);
    
    
    // -- Generate a new random EC key pair with private key called r and public key called R.
    CKey keyR;
    keyR.MakeNewKey(true); // make compressed key
    
    
    // -- Do an EC point multiply with public key K and private key r. This gives you public key P. 
    CKey keyK;
    if (!keyK.SetPubKey(cpkDestK))
    {
        printf("Could not set pubkey for K: %s.\n", ValueString(cpkDestK.Raw()).c_str());
        return 4; // address to is invalid
    };
    
    std::vector<unsigned char> vchP;
    vchP.resize(32);
    EC_KEY* pkeyr = keyR.GetECKey();
    EC_KEY* pkeyK = keyK.GetECKey();
    
    // always seems to be 32, worth checking?
    //int field_size = EC_GROUP_get_degree(EC_KEY_get0_group(pkeyr));
    //int secret_len = (field_size+7)/8;
    //printf("secret_len %d.\n", secret_len);
    
    // -- ECDH_compute_key returns the same P if fed compressed or uncompressed public keys
    ECDH_set_method(pkeyr, ECDH_OpenSSL());
    int lenP = ECDH_compute_key(&vchP[0], 32, EC_KEY_get0_public_key(pkeyK), pkeyr, NULL);
    
    if (lenP != 32)
    {
        printf("ECDH_compute_key failed, lenP: %d.\n", lenP);
        return 6;
    };
    
    CPubKey cpkR = keyR.GetPubKey();
    if (!cpkR.IsValid()
        || !cpkR.IsCompressed())
    {
        printf("Could not get public key for key R.\n");
        return 1;
    };
    
    memcpy(smsg.cpkR, &cpkR.Raw()[0], 33);
    
    
    // -- Use public key P and calculate the SHA512 hash H.
    //    The first 32 bytes of H are called key_e and the last 32 bytes are called key_m.
    std::vector<unsigned char> vchHashed;
    vchHashed.resize(64); // 512
    SHA512(&vchP[0], vchP.size(), (unsigned char*)&vchHashed[0]);
    std::vector<unsigned char> key_e(&vchHashed[0], &vchHashed[0]+32);
    std::vector<unsigned char> key_m(&vchHashed[32], &vchHashed[32]+32);
    
    
    std::vector<unsigned char> vchPayload;
    std::vector<unsigned char> vchCompressed;
    unsigned char* pMsgData;
    uint32_t lenMsgData;
    
    uint32_t lenMsg = message.size();
    if (lenMsg > 128)
    {
        // -- only compress if over 128 bytes
        int worstCase = LZ4_compressBound(message.size());
        vchCompressed.resize(worstCase);
        int lenComp = LZ4_compress((char*)message.c_str(), (char*)&vchCompressed[0], lenMsg);
        if (lenComp < 1)
        {
            printf("Could not compress message data.\n");
            return 9;
        };
        
        pMsgData = &vchCompressed[0];
        lenMsgData = lenComp;
        
    } else
    {
        // -- no compression
        pMsgData = (unsigned char*)message.c_str();
        lenMsgData = lenMsg;
    };
    
    if (fSendAnonymous)
    {
        vchPayload.resize(9 + lenMsgData);
        memcpy(&vchPayload[9], pMsgData, lenMsgData);
        
        vchPayload[0] = 250; // id as anonymous message
        // -- next 4 bytes are unused - there to ensure encrypted payload always > 8 bytes
        memcpy(&vchPayload[5], &lenMsg, 4); // length of uncompressed plain text
    } else
    {
        vchPayload.resize(SMSG_PL_HDR_LEN + lenMsgData);
        memcpy(&vchPayload[SMSG_PL_HDR_LEN], pMsgData, lenMsgData);
        // -- compact signature proves ownership of from address and allows the public key to be recovered, recipient can always reply.
        if (!pwalletMain->GetKey(ckidFrom, keyFrom))
        {
            printf("Could not get private key for addressFrom.\n");
            return 7;
        };
        
        // -- sign the plaintext
        std::vector<unsigned char> vchSignature;
        vchSignature.resize(65);
        keyFrom.SignCompact(Hash(message.begin(), message.end()), vchSignature);
        
        // -- Save some bytes by sending address raw
        vchPayload[0] = (static_cast<CBitcoinAddress_B*>(&coinAddrFrom))->getVersion(); // vchPayload[0] = coinAddrDest.nVersion;
        memcpy(&vchPayload[1], (static_cast<CKeyID_B*>(&ckidFrom))->GetPPN(), 20); // memcpy(&vchPayload[1], ckidDest.pn, 20);
        
        memcpy(&vchPayload[1+20], &vchSignature[0], vchSignature.size());
        memcpy(&vchPayload[1+20+65], &lenMsg, 4); // length of uncompressed plain text
    };
    
    
    SMsgCrypter crypter;
    crypter.SetKey(key_e, smsg.iv);
    std::vector<unsigned char> vchCiphertext;
    
    if (!crypter.Encrypt(&vchPayload[0], vchPayload.size(), vchCiphertext))
    {
        printf("crypter.Encrypt failed.\n");
        return 11;
    };
    
    try {
        smsg.pPayload = new unsigned char[vchCiphertext.size()];
    } catch (std::exception& e)
    {
        printf("Could not allocate pPayload, exception: %s.\n", e.what());
        return 8;
    };
    
    memcpy(smsg.pPayload, &vchCiphertext[0], vchCiphertext.size());
    smsg.nPayload = vchCiphertext.size();
    
    
    // -- Calculate a 32 byte MAC with HMACSHA256, using key_m as salt
    //    Message authentication code, (hash of timestamp + destination + payload)
    bool fHmacOk = true;
    unsigned int nBytes = 32;
    HMAC_CTX ctx;
    HMAC_CTX_init(&ctx);
    
    if (!HMAC_Init_ex(&ctx, &key_m[0], 32, EVP_sha256(), NULL)
        || !HMAC_Update(&ctx, (unsigned char*) &smsg.timestamp, sizeof(smsg.timestamp))
        || !HMAC_Update(&ctx, (unsigned char*) &smsg.destHash[0], sizeof(smsg.destHash))
        || !HMAC_Update(&ctx, &vchCiphertext[0], vchCiphertext.size())
        || !HMAC_Final(&ctx, smsg.mac, &nBytes)
        || nBytes != 32)
        fHmacOk = false;
    
    HMAC_CTX_cleanup(&ctx);
    
    if (!fHmacOk)
    {
        printf("Could not generate MAC.\n");
        return 10;
    };
    
    
    return 0;
};

int SecureMsgSend(std::string& addressFrom, std::string& addressTo, std::string& message, std::string& sError)
{
    /* Encrypt secure message, and place it on the network
        Make a copy of the message to sender's first address and place in send queue db
        proof of work thread will pick up messages from  send queue db
        
    */
    
    if (fDebugSmsg)
        printf("SecureMsgSend(%s, %s, ...)\n", addressFrom.c_str(), addressTo.c_str());
    
    if (message.size() > SMSG_MAX_MSG_BYTES)
    {
        std::ostringstream oss;
        oss << message.size() << " > " << SMSG_MAX_MSG_BYTES;
        sError = "Message is too long, " + oss.str();
        printf("Message is too long, %"PRIszu".\n", message.size());
        return 1;
    };
    
    
    int rv;
    SecureMessage smsg;
    
    if ((rv = SecureMsgEncrypt(smsg, addressFrom, addressTo, message)) != 0)
    {
        printf("SecureMsgSend(), encrypt for recipient failed.\n");
        
        switch(rv)
        {
            case 2:  sError = "Message is too long.";                       break;
            case 3:  sError = "Invalid addressFrom.";                       break;
            case 4:  sError = "Invalid addressTo.";                         break;
            case 5:  sError = "Could not get public key for addressTo.";    break;
            case 6:  sError = "ECDH_compute_key failed.";                   break;
            case 7:  sError = "Could not get private key for addressFrom."; break;
            case 8:  sError = "Could not allocate memory.";                 break;
            case 9:  sError = "Could not compress message data.";           break;
            case 10: sError = "Could not generate MAC.";                    break;
            case 11: sError = "Encrypt failed.";                            break;
            default: sError = "Unspecified Error.";                         break;
        };
        
        return rv;
    };
    
    
    // -- Place message in send queue, proof of work will happen in a thread.
    {
        LOCK(cs_smsgSendQueue);
        
        CSmesgSendQueueDB dbSendQueue("cw");
        
        std::vector<unsigned char> vchKey;
        vchKey.resize(16);
        memcpy(&vchKey[0], &smsg.hash[0] + 5, 8);   // timestamp
        memcpy(&vchKey[8], &smsg.pPayload, 8);      // sample
        
        SecOutboxMsg smsgToSendQueue;
        
        smsgToSendQueue.timeReceived  = GetTime();
        smsgToSendQueue.sAddrTo       = addressTo;
        //smsgOutbox.sAddrOutbox   = addressOutbox;
        
        smsgToSendQueue.vchMessage.resize(SMSG_HDR_LEN + smsg.nPayload);
        memcpy(&smsgToSendQueue.vchMessage[0], &smsg.hash[0], SMSG_HDR_LEN);
        memcpy(&smsgToSendQueue.vchMessage[SMSG_HDR_LEN], smsg.pPayload, smsg.nPayload);
        
        dbSendQueue.WriteSmesg(vchKey, smsgToSendQueue);
        
        dbSendQueue.Close();
        //NotifySecMsgSendQueueChanged(smsgOutbox);
    }
    
    // TODO: only update outbox when proof of work thread is done.
    
    /*
    // -- proof of work only for sent copy
    if (SecureMsgSetHash(&smsg.hash[0], smsg.pPayload, smsg.nPayload) != 0)
    {
        sError = "Could not get proof of work hash.";
        printf("%s\n", sError.c_str());
        return 12;
    };
    
    // -- add to message store
    {
        LOCK(cs_smsg);
        if (SecureMsgStore(smsg, true) != 0)
        {
            sError = "Could not store message.";
            return 1;
        };
    }
    
    // -- test if message was sent to self
    if (SecureMsgScanMessage(&smsg.hash[0], smsg.pPayload, smsg.nPayload) != 0)
    {
        // message recipient is not this node (or failed)
    };
    */
    
    
    //  -- for outbox create a copy encrypted for owned address
    //     if the wallet is encrypted private key needed to decrypt will be unavailable
    
    if (fDebugSmsg)
        printf("Encrypting message for outbox.\n");
    
    std::string addressOutbox;
    CBitcoinAddress coinAddrOutbox;
    
    BOOST_FOREACH(const PAIRTYPE(CTxDestination, std::string)& entry, pwalletMain->mapAddressBook)
        {
        if (!IsMine(*pwalletMain, entry.first))
            continue;

        const CBitcoinAddress& address = entry.first;

        addressOutbox = address.ToString();
        if (!coinAddrOutbox.SetString(addressOutbox)) // test valid
        {
            continue;
        };
        //if (strName == "" || strName == "0") // just get first valid address (what happens if user renames account)
        break;
    };

    if (fDebugSmsg)
        printf("Encrypting a copy for outbox, using address %s\n", addressOutbox.c_str());
    
    SecureMessage smsgForOutbox;
    if ((rv = SecureMsgEncrypt(smsgForOutbox, addressFrom, addressOutbox, message)) != 0)
    {
        printf("SecureMsgSend(), encrypt for outbox failed, %d.\n", rv);
    } else
    {
        // -- save to outbox db
        {
            LOCK(cs_smsgOutbox);
            
            CSmesgOutboxDB dbOutbox("cw");
            
            std::vector<unsigned char> vchKey;
            vchKey.resize(16); // timestamp8 + sample8
            memcpy(&vchKey[0], &smsgForOutbox.hash[0] + 5, 8);   // timestamp
            memcpy(&vchKey[8], &smsgForOutbox.pPayload, 8);  // sample
            
            SecOutboxMsg smsgOutbox;
            
            smsgOutbox.timeReceived  = GetTime();
            smsgOutbox.sAddrTo       = addressTo;
            smsgOutbox.sAddrOutbox   = addressOutbox;
            
            smsgOutbox.vchMessage.resize(SMSG_HDR_LEN + smsgForOutbox.nPayload);
            memcpy(&smsgOutbox.vchMessage[0], &smsgForOutbox.hash[0], SMSG_HDR_LEN);
            memcpy(&smsgOutbox.vchMessage[SMSG_HDR_LEN], smsgForOutbox.pPayload, smsgForOutbox.nPayload);
            
            
            dbOutbox.WriteSmesg(vchKey, smsgOutbox);
            
            NotifySecMsgOutboxChanged(smsgOutbox);
        }
    };
    
    
    if (fDebugSmsg)
        printf("Secure message queued for sending to %s.\n", addressTo.c_str());
    
    return 0;
};


int SecureMsgDecrypt(bool fTestOnly, std::string& address, unsigned char *pHeader, unsigned char *pPayload, uint32_t nPayload, MessageData& msg)
{
    /* Decrypt secure message
        
        address is the owned address to decrypt with.
        
        validate first in SecureMsgValidate
        
        returns
            1       Error
            2       Unknown version number
            3       Decrypt address is not valid.
    */
    
    if (fDebugSmsg)
        printf("SecureMsgDecrypt(), using %s, testonly %d.\n", address.c_str(), fTestOnly);
    
    if (!pHeader
        || !pPayload)
    {
        printf("Error: null pointer to header or payload.\n");
        return 1;
    };
    
    SecureMessage* psmsg = (SecureMessage*) pHeader;
    
    
    if (psmsg->version != 1)
    {
        printf("Unknown version number.\n");
        return 2;
    };
    
    
    
    // -- Fetch private key k, used to decrypt
    CBitcoinAddress coinAddrDest;
    CKeyID ckidDest;
    CKey keyDest;
    if (!coinAddrDest.SetString(address))
    {
        printf("Address is not valid.\n");
        return 3;
    };
    if (!coinAddrDest.GetKeyID(ckidDest))
    {
        printf("coinAddrDest.GetKeyID failed: %s.\n", coinAddrDest.ToString().c_str());
        return 3;
    };
    if (!pwalletMain->GetKey(ckidDest, keyDest))
    {
        printf("Could not get private key for addressDest.\n");
        return 3;
    };
    
    
    
    CKey keyR;
    std::vector<unsigned char> vchR(psmsg->cpkR, psmsg->cpkR+33); // would be neater to override CPubKey() instead
    CPubKey cpkR(vchR);
    if (!cpkR.IsValid())
    {
        printf("Could not get public key for key R.\n");
        return 1;
    };
    if (!keyR.SetPubKey(cpkR))
    {
        printf("Could not set pubkey for R: %s.\n", ValueString(cpkR.Raw()).c_str());
        return 1;
    };
    
    cpkR = keyR.GetPubKey();
    if (!cpkR.IsValid()
        || !cpkR.IsCompressed())
    {
        printf("Could not get compressed public key for key R.\n");
        return 1;
    };
    
    
    // -- Do an EC point multiply with private key k and public key R. This gives you public key P.
    std::vector<unsigned char> vchP;
    vchP.resize(32);
    EC_KEY* pkeyk = keyDest.GetECKey();
    EC_KEY* pkeyR = keyR.GetECKey();
    
    ECDH_set_method(pkeyk, ECDH_OpenSSL());
    int lenPdec = ECDH_compute_key(&vchP[0], 32, EC_KEY_get0_public_key(pkeyR), pkeyk, NULL);
    
    if (lenPdec != 32)
    {
        printf("ECDH_compute_key failed, lenPdec: %d.\n", lenPdec);
        return 1;
    };
    
    
    // -- Use public key P to calculate the SHA512 hash H. 
    //    The first 32 bytes of H are called key_e and the last 32 bytes are called key_m. 
    std::vector<unsigned char> vchHashedDec;
    vchHashedDec.resize(64);    // 512 bits
    SHA512(&vchP[0], vchP.size(), (unsigned char*)&vchHashedDec[0]);
    std::vector<unsigned char> key_e(&vchHashedDec[0], &vchHashedDec[0]+32);
    std::vector<unsigned char> key_m(&vchHashedDec[32], &vchHashedDec[32]+32);
    
    
    // -- Message authentication code, (hash of timestamp + destination + payload)
    unsigned char MAC[32];
    bool fHmacOk = true;
    unsigned int nBytes = 32;
    HMAC_CTX ctx;
    HMAC_CTX_init(&ctx);
    
    if (!HMAC_Init_ex(&ctx, &key_m[0], 32, EVP_sha256(), NULL)
        || !HMAC_Update(&ctx, (unsigned char*) &psmsg->timestamp, sizeof(psmsg->timestamp))
        || !HMAC_Update(&ctx, (unsigned char*) &psmsg->destHash[0], sizeof(psmsg->destHash))
        || !HMAC_Update(&ctx, pPayload, nPayload)
        || !HMAC_Final(&ctx, MAC, &nBytes)
        || nBytes != 32)
        fHmacOk = false;
    
    HMAC_CTX_cleanup(&ctx);
    
    if (!fHmacOk)
    {
        printf("Could not generate MAC.\n");
        return 1;
    };
    
    if (memcmp(MAC, psmsg->mac, 32) != 0)
    {
        if (fDebugSmsg)
            printf("MAC does not match.\n"); // expected if message is not to address on node
        
        return 1;
    };
    
    if (fTestOnly)
        return 0;
    
    SMsgCrypter crypter;
    crypter.SetKey(key_e, psmsg->iv);
    std::vector<unsigned char> vchPayload;
    if (!crypter.Decrypt(pPayload, nPayload, vchPayload))
    {
        printf("Decrypt failed.\n");
        return 1;
    };
    
    msg.timestamp = psmsg->timestamp;
    uint32_t lenData;
    uint32_t lenPlain;
    
    unsigned char* pMsgData;
    bool fFromAnonymous;
    if ((uint32_t)vchPayload[0] == 250)
    {
        fFromAnonymous = true;
        lenData = vchPayload.size() - (9);
        memcpy(&lenPlain, &vchPayload[5], 4);
        pMsgData = &vchPayload[9];
    } else
    {
        fFromAnonymous = false;
        lenData = vchPayload.size() - (SMSG_PL_HDR_LEN);
        memcpy(&lenPlain, &vchPayload[1+20+65], 4);
        pMsgData = &vchPayload[SMSG_PL_HDR_LEN];
    };
    
    msg.vchMessage.resize(lenPlain + 1);
    
    if (lenPlain > 128)
    {
        // -- decompress
        if (LZ4_decompress_safe((char*) pMsgData, (char*) &msg.vchMessage[0], lenData, lenPlain) != (int) lenPlain)
        {
            printf("Could not decompress message data.\n");
            return 1;
        };
    } else
    {
        // -- plaintext
        memcpy(&msg.vchMessage[0], pMsgData, lenPlain);
    };
    
    msg.vchMessage[lenPlain] = '\0';
    
    if (fFromAnonymous)
    {
        // -- Anonymous sender
        msg.sFromAddress = "anon";
    } else
    {
        std::vector<unsigned char> vchUint160;
        vchUint160.resize(20);
        
        memcpy(&vchUint160[0], &vchPayload[1], 20);
        
        uint160 ui160(vchUint160);
        CKeyID ckidFrom(ui160);
        
        CBitcoinAddress coinAddrFrom;
        coinAddrFrom.Set(ckidFrom);
        if (!coinAddrFrom.IsValid())
        {
            printf("From Addess is invalid.\n");
            return 1;
        };
        
        std::vector<unsigned char> vchSig;
        vchSig.resize(65);
        
        memcpy(&vchSig[0], &vchPayload[1+20], 65);
        
        CKey keyFrom;
        keyFrom.SetCompactSignature(Hash(msg.vchMessage.begin(), msg.vchMessage.end()-1), vchSig);
        CPubKey cpkFromSig = keyFrom.GetPubKey();
        if (!cpkFromSig.IsValid())
        {
            printf("Signature validation failed.\n");
            return 1;
        };
        
        // -- get address for the compressed public key
        CBitcoinAddress coinAddrFromSig;
        coinAddrFromSig.Set(cpkFromSig.GetID());
        
        if (!(coinAddrFrom == coinAddrFromSig))
        {
            printf("Signature validation failed.\n");
            return 1;
        };
        
        cpkFromSig = keyFrom.GetPubKey();
        
        int rv = 5;
        try {
            rv = SecureMsgInsertAddress(ckidFrom, cpkFromSig);
        } catch (std::exception& e) {
            printf("SecureMsgInsertAddress(), exception: %s.\n", e.what());
            //return 1;
        };
        
        switch(rv)
        {
            case 0:
                printf("Sender public key added to db.\n");
                break;
            case 4:
                printf("Sender public key already in db.\n");
                break;
            default:
                printf("Error adding sender public key to db.\n");
                break;
        };
        
        msg.sFromAddress = coinAddrFrom.ToString();
    };
    
    if (fDebugSmsg)
        printf("Decrypted message for %s.\n", address.c_str());
    
    return 0;
};

int SecureMsgDecrypt(bool fTestOnly, std::string& address, SecureMessage& smsg, MessageData& msg)
{
    return SecureMsgDecrypt(fTestOnly, address, &smsg.hash[0], smsg.pPayload, smsg.nPayload, msg);
};

