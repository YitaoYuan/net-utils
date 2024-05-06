// https://github.com/YitaoYuan/net-utils

#include <cstdio>
#include <stdint.h>
#include <vector>
#include <algorithm>
#include <string>
#include <vector>

#include <unistd.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <ifaddrs.h>


using std::string;
using std::vector;

#ifdef ENABLE_IB_UTILS
#include <infiniband/verbs.h>

static void show_devices() {
    ibv_device **dev = ibv_get_device_list(NULL);
    if(dev == NULL) {perror("ibv_get_device_list"); return;}
    printf("Devices:\n");
    for(int id = 0; dev[id]; id++) 
        printf("%s\n", ibv_get_device_name(dev[id]));
    ibv_free_device_list(dev);
}

static ibv_device *find_device(string name) {
    ibv_device **dev = ibv_get_device_list(NULL);
    ibv_device *ret = NULL;
    int id;
    if(dev == NULL) {
        perror("ibv_get_device_list");
        goto free_list;
    }
    for(id = 0; dev[id]; id++) 
        if(!strcmp(ibv_get_device_name(dev[id]), name.c_str())) {
            ret = dev[id];
            break;
        }
free_list:
    ibv_free_device_list(dev);
    return ret;
} 

static union ibv_gid ipv4_to_gid(uint32_t ip)
{
    union ibv_gid gid;
    gid.global.subnet_prefix = 0;
    gid.global.interface_id = htobe64((uint64_t)0x0000ffff00000000ull | (uint64_t)ip);
    return gid;
}

static uint32_t gid_to_ipv4(union ibv_gid gid)
{   
    return (uint32_t)be64toh(gid.global.interface_id);
}

static string gid_to_str(ibv_gid gid)
{
    string s;
    s.reserve(sizeof(gid)/2*3);
    char buf[20];
    int len = sizeof(gid)/sizeof(uint16_t);
    for(int i = 0; i < len; i++) {
        sprintf(buf, "%04x", (uint32_t)ntohs(((uint16_t*)&gid)[i]));
        s += buf;
        if(i < len - 1) s += ':';
    }
    return s;
}

// input ens10f1
// output mlx5_1
static string dev_to_ib_dev(string dev)
{
    FILE* fp = popen("ibdev2netdev", "r");
    char dev_buf[64], ib_dev_buf[64];
    int ret;
    while((ret = fscanf(fp, "%s %*s %*d ==> %s %*[^\n]%*c", ib_dev_buf, dev_buf)) != -1) {
        if(dev == dev_buf) return ib_dev_buf;
    }
    return "";
}

// input mlx5_1
// output ens10f1
static string ib_dev_to_dev(string ib_dev)
{
    FILE* fp = popen("ibdev2netdev", "r");
    char dev_buf[64], ib_dev_buf[64];
    int ret;
    while((ret = fscanf(fp, "%s %*s %*d ==> %s %*[^\n]%*c", ib_dev_buf, dev_buf)) != -1) {
        if(ib_dev == ib_dev_buf) return dev_buf;
    }
    return "";
}
#endif

// input 192.168.1.1
// output ens10f1
static string get_dev_by_ip(string ip_addr)
{
    struct in_addr addr;
    if(inet_aton(ip_addr.c_str(), &addr) == 0)
        return "";
    struct ifaddrs* if_list;
    if (getifaddrs(&if_list) < 0)
        return "";
    string nic_name;
    for (struct ifaddrs *ifa = if_list; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr->sa_family == AF_INET) {
            if (!memcmp(&addr, &(((struct sockaddr_in *) ifa->ifa_addr)->sin_addr), sizeof(struct in_addr))) {
                nic_name = ifa->ifa_name;
                break;
            }
        }
    }
    freeifaddrs(if_list);
    return nic_name;
}

// input : ens10f1
// output : 0000:e3:00.1 域(16位), 总线(8位), 设备(5位00-1f)和功能(3位0-8)
static string get_pci_by_dev(string dev)
{
    int sock = socket(PF_INET, SOCK_DGRAM, 0);

    struct ifreq ifr;
    struct ethtool_cmd cmd;
    struct ethtool_drvinfo drvinfo;

    memset(&ifr, 0, sizeof ifr);
    memset(&cmd, 0, sizeof cmd);
    memset(&drvinfo, 0, sizeof drvinfo);
    strcpy(ifr.ifr_name, dev.c_str());

    ifr.ifr_data = (char*)&drvinfo;
    drvinfo.cmd = ETHTOOL_GDRVINFO;

    string pci;
    if(!(ioctl(sock, SIOCETHTOOL, &ifr) < 0)) {
        pci = drvinfo.bus_info;
    }
    close(sock);
    return pci;
}

// input : 0000:e3:00.1
// output : 1
static int get_socket_by_pci(string pci)
{
    int socket = -1;
    char path[128];
    sprintf(path, "/sys/bus/pci/devices/%s/numa_node", pci.c_str());
    FILE* fp = fopen(path, "r"); 
    if (fp != NULL) {
        fscanf(fp, "%d", &socket);
        fclose(fp);
    }
    return socket;
}

// output : {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1}
static vector<int> get_socket_list_with_cpu_index()
{
    FILE* fp = popen("cat /proc/cpuinfo | grep \"physical id\"", "r");
    if (fp != NULL) {
        vector<int> socket_list;

        int ret, socket_id;
        while((ret = fscanf(fp, "%*s %*s : %d%*[^\n]%*c", &socket_id)) != -1) {
            socket_list.push_back(socket_id);
        }

        fclose(fp);
        return socket_list;
    }
    perror("get_socket_list_with_cpu_index");
    exit(0);
}

// output : {{0, 1, 2, 3, 8, 9, 10, 11}, {4, 5, 6, 7, 12, 13, 14, 15}}
static vector<vector<int>> get_cpu_list_with_socket_index()
{
    vector<int> socket_list = get_socket_list_with_cpu_index();
    vector<vector<int>> cpu_list;
    cpu_list.resize(1 + *max_element(socket_list.begin(), socket_list.end()));
    for(size_t cpu = 0; cpu < socket_list.size(); cpu++) 
        cpu_list[socket_list[cpu]].push_back(cpu);
    return cpu_list;
}

static vector<int> get_cpu_list_by_socket(int socket) 
{
    auto cpu_list = get_cpu_list_with_socket_index();
    if(socket < 0 || socket >= cpu_list.size()) return {};
    return cpu_list[socket];
}
