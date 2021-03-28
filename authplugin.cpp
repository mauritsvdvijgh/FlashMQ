/*
This file is part of FlashMQ (https://www.flashmq.org)
Copyright (C) 2021 Wiebe Cazemier

FlashMQ is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, version 3.

FlashMQ is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public
License along with FlashMQ. If not, see <https://www.gnu.org/licenses/>.
*/

#include "authplugin.h"

#include <unistd.h>
#include <fcntl.h>
#include <sstream>
#include <dlfcn.h>
#include <fstream>
#include "sys/stat.h"

#include "exceptions.h"
#include "unscopedlock.h"
#include "utils.h"

std::mutex Authentication::initMutex;
std::mutex Authentication::authChecksMutex;

void mosquitto_log_printf(int level, const char *fmt, ...)
{
    Logger *logger = Logger::getInstance();
    va_list valist;
    va_start(valist, fmt);
    logger->logf(level, fmt, valist);
    va_end(valist);
}

MosquittoPasswordFileEntry::MosquittoPasswordFileEntry(const std::vector<char> &&salt, const std::vector<char> &&cryptedPassword) :
    salt(salt),
    cryptedPassword(cryptedPassword)
{

}


Authentication::Authentication(Settings &settings) :
    settings(settings),
    mosquittoPasswordFile(settings.mosquittoPasswordFile),
    mosquittoDigestContext(EVP_MD_CTX_new())
{
    logger = Logger::getInstance();

    if(!sha512)
    {
        throw std::runtime_error("Failed to initialize SHA512 for decoding auth entry");
    }

    EVP_DigestInit_ex(mosquittoDigestContext, sha512, NULL);
    memset(&mosquittoPasswordFileLastLoad, 0, sizeof(struct timespec));
}

Authentication::~Authentication()
{
    cleanup();

    if (mosquittoDigestContext)
        EVP_MD_CTX_free(mosquittoDigestContext);
}

void *Authentication::loadSymbol(void *handle, const char *symbol) const
{
    void *r = dlsym(handle, symbol);

    if (r == NULL)
    {
        std::string errmsg(dlerror());
        throw FatalError(errmsg);
    }

    return r;
}

void Authentication::loadPlugin(const std::string &pathToSoFile)
{
    if (pathToSoFile.empty())
        return;

    logger->logf(LOG_NOTICE, "Loading auth plugin %s", pathToSoFile.c_str());

    initialized = false;
    useExternalPlugin = true;

    if (access(pathToSoFile.c_str(), R_OK) != 0)
    {
        std::ostringstream oss;
        oss << "Error loading auth plugin: The file " << pathToSoFile << " is not there or not readable";
        throw FatalError(oss.str());
    }

    void *r = dlopen(pathToSoFile.c_str(), RTLD_NOW|RTLD_GLOBAL);

    if (r == NULL)
    {
        std::string errmsg(dlerror());
        throw FatalError(errmsg);
    }

    version = (F_auth_plugin_version)loadSymbol(r, "mosquitto_auth_plugin_version");

    if (version() != 2)
    {
        throw FatalError("Only Mosquitto plugin version 2 is supported at this time.");
    }

    init_v2 = (F_auth_plugin_init_v2)loadSymbol(r, "mosquitto_auth_plugin_init");
    cleanup_v2 = (F_auth_plugin_cleanup_v2)loadSymbol(r, "mosquitto_auth_plugin_cleanup");
    security_init_v2 = (F_auth_plugin_security_init_v2)loadSymbol(r, "mosquitto_auth_security_init");
    security_cleanup_v2 = (F_auth_plugin_security_cleanup_v2)loadSymbol(r, "mosquitto_auth_security_cleanup");
    acl_check_v2 = (F_auth_plugin_acl_check_v2)loadSymbol(r, "mosquitto_auth_acl_check");
    unpwd_check_v2 = (F_auth_plugin_unpwd_check_v2)loadSymbol(r, "mosquitto_auth_unpwd_check");
    psk_key_get_v2 = (F_auth_plugin_psk_key_get_v2)loadSymbol(r, "mosquitto_auth_psk_key_get");

    initialized = true;
}

/**
 * @brief AuthPlugin::init is like Mosquitto's init(), and is to allow the plugin to init memory. Plugins should not load
 * their authentication data here. That's what securityInit() is for.
 */
void Authentication::init()
{
    if (!useExternalPlugin)
        return;

    UnscopedLock lock(initMutex);
    if (settings.authPluginSerializeInit)
        lock.lock();

    if (quitting)
        return;

    AuthOptCompatWrap &authOpts = settings.getAuthOptsCompat();
    int result = init_v2(&pluginData, authOpts.head(), authOpts.size());
    if (result != 0)
        throw FatalError("Error initialising auth plugin.");
}

void Authentication::cleanup()
{
    if (!cleanup_v2)
        return;

    securityCleanup(false);

    AuthOptCompatWrap &authOpts = settings.getAuthOptsCompat();
    int result = cleanup_v2(pluginData, authOpts.head(), authOpts.size());
    if (result != 0)
        logger->logf(LOG_ERR, "Error cleaning up auth plugin"); // Not doing exception, because we're shutting down anyway.
}

/**
 * @brief AuthPlugin::securityInit initializes the security data, like loading users, ACL tables, etc.
 * @param reloading
 */
void Authentication::securityInit(bool reloading)
{
    if (!useExternalPlugin)
        return;

    UnscopedLock lock(initMutex);
    if (settings.authPluginSerializeInit)
        lock.lock();

    if (quitting)
        return;

    AuthOptCompatWrap &authOpts = settings.getAuthOptsCompat();
    int result = security_init_v2(pluginData, authOpts.head(), authOpts.size(), reloading);
    if (result != 0)
    {
        throw AuthPluginException("Plugin function mosquitto_auth_security_init returned an error. If it didn't log anything, we don't know what it was.");
    }
    initialized = true;
}

void Authentication::securityCleanup(bool reloading)
{
    if (!useExternalPlugin)
        return;

    initialized = false;
    AuthOptCompatWrap &authOpts = settings.getAuthOptsCompat();
    int result = security_cleanup_v2(pluginData, authOpts.head(), authOpts.size(), reloading);

    if (result != 0)
    {
        throw AuthPluginException("Plugin function mosquitto_auth_security_cleanup returned an error. If it didn't log anything, we don't know what it was.");
    }
}

AuthResult Authentication::aclCheck(const std::string &clientid, const std::string &username, const std::string &topic, AclAccess access)
{
    if (!useExternalPlugin)
        return AuthResult::success;

    if (!initialized)
    {
        logger->logf(LOG_ERR, "ACL check wanted, but initialization failed.  Can't perform check.");
        return AuthResult::error;
    }

    UnscopedLock lock(authChecksMutex);
    if (settings.authPluginSerializeAuthChecks)
        lock.lock();

    int result = acl_check_v2(pluginData, clientid.c_str(), username.c_str(), topic.c_str(), static_cast<int>(access));
    AuthResult result_ = static_cast<AuthResult>(result);

    if (result_ == AuthResult::error)
    {
        logger->logf(LOG_ERR, "ACL check by plugin returned error for topic '%s'. If it didn't log anything, we don't know what it was.", topic.c_str());
    }

    return result_;
}

AuthResult Authentication::unPwdCheck(const std::string &username, const std::string &password)
{
    AuthResult firstResult = unPwdCheckFromMosquittoPasswordFile(username, password);

    if (firstResult != AuthResult::success)
        return firstResult;

    if (!useExternalPlugin)
        return firstResult;

    if (!initialized)
    {
        logger->logf(LOG_ERR, "Username+password check with plugin wanted, but initialization failed. Can't perform check.");
        return AuthResult::error;
    }

    UnscopedLock lock(authChecksMutex);
    if (settings.authPluginSerializeAuthChecks)
        lock.lock();

    int result = unpwd_check_v2(pluginData, username.c_str(), password.c_str());
    AuthResult r = static_cast<AuthResult>(result);

    if (r == AuthResult::error)
    {
        logger->logf(LOG_ERR, "Username+password check by plugin returned error for user '%s'. If it didn't log anything, we don't know what it was.", username.c_str());
    }

    return r;
}

void Authentication::setQuitting()
{
    this->quitting = true;
}

/**
 * @brief Authentication::loadMosquittoPasswordFile is called once on startup, and on a frequent interval, and reloads the file if changed.
 */
void Authentication::loadMosquittoPasswordFile()
{
    if (this->mosquittoPasswordFile.empty())
        return;

    if (access(this->mosquittoPasswordFile.c_str(), R_OK) != 0)
    {
        logger->logf(LOG_ERR, "Passwd file '%s' is not there or not readable.", this->mosquittoPasswordFile.c_str());
        return;
    }

    struct stat statbuf;
    memset(&statbuf, 0, sizeof(struct stat));
    check<std::runtime_error>(stat(mosquittoPasswordFile.c_str(), &statbuf));
    struct timespec ctime = statbuf.st_ctim;

    if (ctime.tv_sec == this->mosquittoPasswordFileLastLoad.tv_sec)
        return;

    logger->logf(LOG_NOTICE, "Change detected in '%s'. Reloading.", this->mosquittoPasswordFile.c_str());

    try
    {
        std::ifstream infile(this->mosquittoPasswordFile, std::ios::in);
        std::unique_ptr<std::unordered_map<std::string, MosquittoPasswordFileEntry>> passwordEntries_tmp(new std::unordered_map<std::string, MosquittoPasswordFileEntry>());

        for(std::string line; getline(infile, line ); )
        {
            if (line.empty())
                continue;

            try
            {
                std::vector<std::string> fields = splitToVector(line, ':');

                if (fields.size() != 2)
                    throw std::runtime_error(formatString("Passwd file line '%s' contains more than one ':'", line.c_str()));

                const std::string &username = fields[0];

                for (const std::string &field : fields)
                {
                    if (field.size() == 0)
                    {
                        throw std::runtime_error(formatString("An empty field was found in '%'", line.c_str()));
                    }
                }

                std::vector<std::string> fields2 = splitToVector(fields[1], '$', 3, false);

                if (fields2.size() != 3)
                    throw std::runtime_error(formatString("Invalid line format in '%s'. Expected three fields separated by '$'", line.c_str()));

                if (fields2[0] != "6")
                    throw std::runtime_error("Password fields must start with $6$");

                std::vector<char> salt = base64Decode(fields2[1]);
                std::vector<char> cryptedPassword = base64Decode(fields2[2]);
                passwordEntries_tmp->emplace(username, MosquittoPasswordFileEntry(std::move(salt), std::move(cryptedPassword)));
            }
            catch (std::exception &ex)
            {
                std::string lineCut = formatString("%s...", line.substr(0, 20).c_str());
                logger->logf(LOG_ERR, "Dropping invalid username/password line: '%s'. Error: %s", lineCut.c_str(), ex.what());
            }
        }

        this->mosquittoPasswordEntries = std::move(passwordEntries_tmp);
        this->mosquittoPasswordFileLastLoad = ctime;
    }
    catch (std::exception &ex)
    {
        logger->logf(LOG_ERR, "Error loading Mosquitto password file: '%s'. Authentication won't work.", ex.what());
    }
}

AuthResult Authentication::unPwdCheckFromMosquittoPasswordFile(const std::string &username, const std::string &password)
{
    if (this->mosquittoPasswordFile.empty())
        return AuthResult::success;

    if (!this->mosquittoPasswordEntries)
        return AuthResult::login_denied;

    AuthResult result = settings.allowAnonymous ? AuthResult::success : AuthResult::login_denied;

    auto it = mosquittoPasswordEntries->find(username);
    if (it != mosquittoPasswordEntries->end())
    {
        result = AuthResult::login_denied;

        unsigned char md_value[EVP_MAX_MD_SIZE];
        unsigned int output_len = 0;

        const MosquittoPasswordFileEntry &entry = it->second;

        EVP_MD_CTX_reset(mosquittoDigestContext);
        EVP_DigestInit_ex(mosquittoDigestContext, sha512, NULL);
        EVP_DigestUpdate(mosquittoDigestContext, password.c_str(), password.length());
        EVP_DigestUpdate(mosquittoDigestContext, entry.salt.data(), entry.salt.size());
        EVP_DigestFinal_ex(mosquittoDigestContext, md_value, &output_len);

        std::vector<char> hashedSalted(output_len);
        std::memcpy(hashedSalted.data(), md_value, output_len);

        if (hashedSalted == entry.cryptedPassword)
            result = AuthResult::success;
    }

    return result;
}

std::string AuthResultToString(AuthResult r)
{
    {
        if (r == AuthResult::success)
            return "success";
        if (r == AuthResult::acl_denied)
            return "ACL denied";
        if (r == AuthResult::login_denied)
            return "login Denied";
        if (r == AuthResult::error)
            return "error in check";
    }

    return "";
}
