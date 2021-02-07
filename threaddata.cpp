#include "threaddata.h"
#include <string>
#include <sstream>

ThreadData::ThreadData(int threadnr, std::shared_ptr<SubscriptionStore> &subscriptionStore, ConfigFileParser &confFileParser) :
    subscriptionStore(subscriptionStore),
    confFileParser(confFileParser),
    authPlugin(confFileParser),
    threadnr(threadnr)
{
    logger = Logger::getInstance();

    epollfd = check<std::runtime_error>(epoll_create(999));
}

void ThreadData::start(thread_f f)
{
    this->thread = std::thread(f, this);

    pthread_t native = this->thread.native_handle();
    std::ostringstream threadName;
    threadName << "FlashMQ T " << threadnr;
    threadName.flush();
    const char *c_str = threadName.str().c_str();
    pthread_setname_np(native, c_str);

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(threadnr, &cpuset);
    check<std::runtime_error>(pthread_setaffinity_np(native, sizeof(cpuset), &cpuset));

    // It's not really necessary to get affinity again, but now I'm logging truth instead assumption.
    check<std::runtime_error>(pthread_getaffinity_np(native, sizeof(cpuset), &cpuset));
    int pinned_cpu = -1;
    for (int j = 0; j < CPU_SETSIZE; j++)
        if (CPU_ISSET(j, &cpuset))
            pinned_cpu = j;

    logger->logf(LOG_NOTICE, "Thread '%s' pinned to CPU %d", c_str, pinned_cpu);
}

void ThreadData::quit()
{
    running = false;
    thread.join();
}

void ThreadData::giveClient(Client_p client)
{
    clients_by_fd_mutex.lock();
    int fd = client->getFd();
    clients_by_fd[fd] = client;
    clients_by_fd_mutex.unlock();

    struct epoll_event ev;
    memset(&ev, 0, sizeof (struct epoll_event));
    ev.data.fd = fd;
    ev.events = EPOLLIN;
    check<std::runtime_error>(epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev));
}

Client_p ThreadData::getClient(int fd)
{
    std::lock_guard<std::mutex> lck(clients_by_fd_mutex);
    return this->clients_by_fd[fd];
}

void ThreadData::removeClient(Client_p client)
{
    client->markAsDisconnecting();

    std::lock_guard<std::mutex> lck(clients_by_fd_mutex);
    clients_by_fd.erase(client->getFd());
}

void ThreadData::removeClient(int fd)
{
    std::lock_guard<std::mutex> lck(clients_by_fd_mutex);
    auto client_it = this->clients_by_fd.find(fd);
    if (client_it != this->clients_by_fd.end())
    {
        client_it->second->markAsDisconnecting();
        this->clients_by_fd.erase(fd);
    }
}

std::shared_ptr<SubscriptionStore> &ThreadData::getSubscriptionStore()
{
    return subscriptionStore;
}

// TODO: profile how fast hash iteration is. Perhaps having a second list/vector is beneficial?
bool ThreadData::doKeepAliveCheck()
{
    std::unique_lock<std::mutex> lock(clients_by_fd_mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return false;

    auto it = clients_by_fd.begin();
    while (it != clients_by_fd.end())
    {
        Client_p &client = it->second;
        if (client && client->keepAliveExpired())
        {
            client->setDisconnectReason("Keep-alive expired: " + client->getKeepAliveInfoString());
            it = clients_by_fd.erase(it);
        }
        else
            it++;
    }

    return true;
}

void ThreadData::initAuthPlugin()
{
    authPlugin.loadPlugin(confFileParser.authPluginPath);
    authPlugin.init();
    authPlugin.securityInit(false);
}

void ThreadData::reload()
{
    try
    {
        authPlugin.securityCleanup(true);
        authPlugin.securityInit(true);
    }
    catch (AuthPluginException &ex)
    {
        logger->logf(LOG_ERR, "Error reloading auth plugin: %s. Security checks will now fail, because we don't know the status of the plugin anymore.", ex.what());
    }
}



