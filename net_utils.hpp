// https://github.com/YitaoYuan/net-utils

#ifndef __NET_UTILS_HPP__
#define __NET_UTILS_HPP__

#include <cstdio>
#include <stdint.h>
#include <vector>
#include <algorithm>
#include <string>
#include <vector>
#include <tuple>

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

static void show_ib_devices() {
    ibv_device **dev = ibv_get_device_list(NULL);
    if(dev == NULL) {perror("ibv_get_device_list"); return;}
    printf("Devices:\n");
    for(int id = 0; dev[id]; id++) 
        printf("%s\n", ibv_get_device_name(dev[id]));
    ibv_free_device_list(dev);
}

bool get_gid_index(uint32_t ip, ibv_context *ctx, uint8_t *port_id, uint8_t *gid_index)
{
    ibv_device_attr dev_attr;
    ibv_port_attr port_attr;
    int ret = ibv_query_device(ctx, &dev_attr);
    if(ret) {printf("Query device failed\n"); return false;}
    for(uint8_t i = 1; i <= dev_attr.phys_port_cnt; i++) {
        ret = ibv_query_port(ctx, i, &port_attr);
        if(ret) {printf("Query port %d failed\n", (int)i); continue;}
        for(int j = 1; j < port_attr.gid_tbl_len; j += 2) {// only query RoCEv2
            ibv_gid gid;
            ret = ibv_query_gid(ctx, i, j, &gid);
            if(gid_to_ipv4(gid) != ip) continue;
            *port_id = i;
            *gid_index = j;
            return true;
        }
    }
    return false;
}

bool query_ib_device_by_ip(uint32_t ip, ibv_device **dev, uint8_t *port_id, uint8_t *gid_index)
{
    ibv_device **dev_list = ibv_get_device_list(NULL);
    bool found = false;
    for(int id = 0; dev_list[id] && !found; id++) {
        ibv_context *ctx = ibv_open_device(dev_list[id]);
        if(ctx == NULL) {
            printf("Open device %d failed\n", id);
            continue;
        }
        if(get_gid_index(ip, ctx, port_id, gid_index)) {
            *dev = dev_list[id];
            found = true;
        }
        if(ibv_close_device(ctx) != 0) {
            printf("Close device %d failed\n", id);
        }
    }
    ibv_free_device_list(dev_list);
    return found;
}

static ibv_device *find_ib_device(string name) {
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

static std::tuple<string, uint32_t> get_device_by_ip(uint32_t ip_addr, uint32_t prefix_mask = (uint32_t)-1)
{
    // struct in_addr addr;
    // addr.s_addr = htonl(ip_addr);
    struct ifaddrs* if_list;
    if (getifaddrs(&if_list) < 0)
        return std::tuple<string, uint32_t>{"", 0};
    string nic_name;
    uint32_t nic_ip_addr;
    for (struct ifaddrs *ifa = if_list; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct in_addr nic_addr = ((struct sockaddr_in *) ifa->ifa_addr)->sin_addr;
            nic_ip_addr = ntohl(nic_addr.s_addr);
            if((ip_addr & prefix_mask) == (nic_ip_addr & prefix_mask)) {
                nic_name = ifa->ifa_name;
                break;
            }
            // if (!memcmp(&addr, &(((struct sockaddr_in *) ifa->ifa_addr)->sin_addr), sizeof(struct in_addr))) {
            //     nic_name = ifa->ifa_name;
            //     break;
            // }
        }
    }
    freeifaddrs(if_list);
    return std::tuple<string, uint32_t>{nic_name, nic_ip_addr};
}

// input 192.168.1.0/24 or 192.168.1.1
// output ens10f1
static std::tuple<string, uint32_t> get_device_by_ip(string ip_addr)
{
    for(size_t i = 0; i < ip_addr.size(); i++) {
        if(ip_addr[i] == '/') {
            string ip_string = ip_addr.substr(0, i); // pos, cnt
            string prefix_len_string(ip_addr.begin() + i + 1, ip_addr.end());
            uint32_t ip = ntohl(inet_addr(ip_string.c_str()));
            uint32_t prefix_len = stoi(prefix_len_string);
            uint32_t prefix_mask = (uint32_t)-1 << (32 - prefix_len);
            return get_device_by_ip(ip, prefix_mask);
        }
    }
    uint32_t ip = ntohl(inet_addr(ip_addr.c_str()));
    return get_device_by_ip(ip);
    // struct in_addr addr;
    // if(inet_aton(ip_addr.c_str(), &addr) == 0)
    //     return "";
    // struct ifaddrs* if_list;
    // if (getifaddrs(&if_list) < 0)
    //     return "";
    // string nic_name;
    // for (struct ifaddrs *ifa = if_list; ifa != NULL; ifa = ifa->ifa_next) {
    //     if (ifa->ifa_addr->sa_family == AF_INET) {
    //         if (!memcmp(&addr, &(((struct sockaddr_in *) ifa->ifa_addr)->sin_addr), sizeof(struct in_addr))) {
    //             nic_name = ifa->ifa_name;
    //             break;
    //         }
    //     }
    // }
    // freeifaddrs(if_list);
    // return nic_name;
}

// input : ens10f1
// output : 0000:e3:00.1, including field(16 bit), bus(8 bit), device(5 bit, 00-1f) and function(3bit, 0-8)
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
#endif
