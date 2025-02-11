/* $Id$ */
/**
 * This file contains declarations of DHCP config.
 */

#ifndef _CONFIG_H_
# define _CONFIG_H_

#include <iprt/asm-math.h>
#include <iprt/cpp/utils.h>

typedef std::vector<RTMAC> MacAddressContainer;
typedef MacAddressContainer::iterator MacAddressIterator;

typedef std::vector<RTNETADDRIPV4> Ipv4AddressContainer;
typedef Ipv4AddressContainer::iterator Ipv4AddressIterator;
typedef Ipv4AddressContainer::const_iterator Ipv4AddressConstIterator;

static bool operator <(const RTNETADDRIPV4& a, const RTNETADDRIPV4& b)
{
    return (RT_N2H_U32(a.u) < RT_N2H_U32(b.u));
}

static bool operator > (const RTNETADDRIPV4& a, const RTNETADDRIPV4& b)
{
    return (b < a);
}


class RawOption
{
public:
    uint8_t u8OptId;
    uint8_t cbRawOpt;
    uint8_t au8RawOpt[255];
};


class Client;
class Lease;
class BaseConfigEntity;


class NetworkConfigEntity;
class HostConfigEntity;
class ClientMatchCriteria;

typedef std::map<Lease *, RTNETADDRIPV4> MapLease2Ip4Address;
typedef MapLease2Ip4Address::iterator MapLease2Ip4AddressIterator;
typedef MapLease2Ip4Address::value_type MapLease2Ip4AddressPair;

/*
 * it's a basic representation of
 * of out undestanding what client is
 * XXX: Client might sends Option 61 (RFC2132 9.14 "Client-identifier") signalling
 * that we may identify it in special way
 *
 * XXX: Client might send Option 60 (RFC2132 9.13 "Vendor class undentifier")
 * in response it's expected server sends Option 43 (RFC2132 8.4. "Vendor Specific Information")
 */
class Client
{
    public:

    /* XXX: Option 60 and 61 */
    Client(const RTMAC& mac);

    bool operator== (const RTMAC& mac) const
    {
        return (  m_mac.au16[0] == mac.au16[0]
                && m_mac.au16[1] == mac.au16[1]
                && m_mac.au16[2] == mac.au16[2]);
    }
    /** Dumps client query */
    void dump();

    /* XXX! private: */

    RTMAC m_mac;
    Lease *m_lease;

    /* XXX: should be in lease */
    std::vector<RawOption> rawOptions;
};


typedef std::vector<Client*> VecClient;
typedef VecClient::iterator VecClientIterator;
typedef VecClient::const_iterator VecClientConstIterator;


/**
 *
 */
class ClientMatchCriteria
{
    public:
    virtual bool check(const Client& client) const {return false;};
};


class ORClientMatchCriteria: ClientMatchCriteria
{
    ClientMatchCriteria* m_left;
    ClientMatchCriteria* m_right;
    ORClientMatchCriteria(ClientMatchCriteria *left, ClientMatchCriteria *right)
    {
        m_left = left;
        m_right = right;
    }

    virtual bool check(const Client& client) const
    {
        return (m_left->check(client) || m_right->check(client));
    }
};


class ANDClientMatchCriteria: ClientMatchCriteria
{
    ClientMatchCriteria* m_left;
    ClientMatchCriteria* m_right;
    ANDClientMatchCriteria(ClientMatchCriteria *left, ClientMatchCriteria *right)
    {
        m_left = left;
        m_right = right;
    }

    virtual bool check(const Client& client) const
    {
        return (m_left->check(client) && m_right->check(client));
    }
};


class AnyClientMatchCriteria: public ClientMatchCriteria
{
    virtual bool check(const Client& client) const
    {
        return true;
    }
};


class MACClientMatchCriteria: public ClientMatchCriteria
{

    public:
    MACClientMatchCriteria(const RTMAC& mac):m_mac(mac){}

    virtual bool check(const Client& client) const
    {
        return (client == m_mac);
    }
    private:
    RTMAC m_mac;
};


#if 0
/* XXX: Later */
class VmSlotClientMatchCriteria: public ClientMatchCriteria
{
    str::string VmName;
    uint8_t u8Slot;
    virtual bool check(const Client& client)
    {
        return (   client.VmName == VmName
                   && (   u8Slot == (uint8_t)~0 /* any */
                       || client.u8Slot == u8Slot));
    }
};
#endif


/* Option 60 */
class ClassClientMatchCriteria: ClientMatchCriteria{};
/* Option 61 */
class ClientIdentifierMatchCriteria: ClientMatchCriteria{};


class BaseConfigEntity
{
public:
    BaseConfigEntity(const ClientMatchCriteria *criteria = NULL,
      int matchingLevel = 0)
      : m_criteria(criteria),
      m_MatchLevel(matchingLevel){};
    virtual ~BaseConfigEntity(){};
    /* XXX */
    int add(BaseConfigEntity *cfg)
    {
        m_children.push_back(cfg);
        return 0;
    }

    /* Should return how strong matching */
    virtual int match(Client& client, BaseConfigEntity **cfg)
    {
        int iMatch = (m_criteria && m_criteria->check(client)? m_MatchLevel: 0);
        if (m_children.empty())
        {
            if (iMatch > 0)
            {
                *cfg = this;
                return iMatch;
            }
        }
        else
        {
            *cfg = this;
            /* XXX: hack */
            BaseConfigEntity *matching = this;
            int matchingLevel = m_MatchLevel;

            for (std::vector<BaseConfigEntity *>::iterator it = m_children.begin();
                 it != m_children.end();
                 ++it)
            {
                iMatch = (*it)->match(client, &matching);
                if (iMatch > matchingLevel)
                {
                    *cfg = matching;
                    matchingLevel = iMatch;
                }
            }
            return matchingLevel;
        }
        return iMatch;
    }
    virtual uint32_t expirationPeriod() const = 0;
    protected:
    const ClientMatchCriteria *m_criteria;
    int m_MatchLevel;
    std::vector<BaseConfigEntity *> m_children;
};


class NullConfigEntity: public BaseConfigEntity
{
public:
    NullConfigEntity(){}
    virtual ~NullConfigEntity(){}
    int add(BaseConfigEntity *cfg) const
    {
        return 0;
    }
    virtual uint32_t expirationPeriod() const {return 0;}
};


class ConfigEntity: public BaseConfigEntity
{
    public:

    /* range */
    /* match conditions */
    ConfigEntity(std::string& name,
                 const BaseConfigEntity *cfg,
                 const ClientMatchCriteria *criteria,
                 int matchingLevel = 0):
      BaseConfigEntity(criteria, matchingLevel),
      m_name(name),
      m_parentCfg(cfg)
    {
        unconst(m_parentCfg)->add(this);
    }

    std::string m_name;
    const BaseConfigEntity *m_parentCfg;
    virtual uint32_t expirationPeriod() const
    {
        if (!m_u32ExpirationPeriod)
            return m_parentCfg->expirationPeriod();
        else
            return m_u32ExpirationPeriod;
    }

    /* XXX: private:*/
    uint32_t m_u32ExpirationPeriod;
};


/**
 * Network specific entries
 */
class NetworkConfigEntity:public ConfigEntity
{
    public:
    /* Address Pool matching with network declaration */
    NetworkConfigEntity(std::string name,
                        const BaseConfigEntity *cfg,
                        const ClientMatchCriteria *criteria,
                        int matchlvl,
                        const RTNETADDRIPV4& networkID,
                        const RTNETADDRIPV4& networkMask,
                        const RTNETADDRIPV4& lowerIP,
                        const RTNETADDRIPV4& upperIP):
      ConfigEntity(name, cfg, criteria, matchlvl),
      m_NetworkID(networkID),
      m_NetworkMask(networkMask),
      m_UpperIP(upperIP),
      m_LowerIP(lowerIP)
    {
    };

    NetworkConfigEntity(std::string name,
                        const BaseConfigEntity *cfg,
                        const ClientMatchCriteria *criteria,
                        const RTNETADDRIPV4& networkID,
                        const RTNETADDRIPV4& networkMask):
      ConfigEntity(name, cfg, criteria, 5),
      m_NetworkID(networkID),
      m_NetworkMask(networkMask)
    {
        m_UpperIP.u = m_NetworkID.u | (~m_NetworkMask.u);
        m_LowerIP.u = m_NetworkID.u;
    };

    const RTNETADDRIPV4& upperIp() const {return m_UpperIP;}
    const RTNETADDRIPV4& lowerIp() const {return m_LowerIP;}
    const RTNETADDRIPV4& networkId() const {return m_NetworkID;}
    const RTNETADDRIPV4& netmask() const {return m_NetworkMask;}

    private:
    RTNETADDRIPV4 m_NetworkID;
    RTNETADDRIPV4 m_NetworkMask;
    RTNETADDRIPV4 m_UpperIP;
    RTNETADDRIPV4 m_LowerIP;
};


/**
 * Host specific entry
 * Address pool is contains one element
 */
class HostConfigEntity: public NetworkConfigEntity
{

    public:
    HostConfigEntity(const RTNETADDRIPV4& addr,
                     std::string name,
                     const NetworkConfigEntity *cfg,
                     const ClientMatchCriteria *criteria):
      NetworkConfigEntity(name,
                          static_cast<const ConfigEntity*>(cfg),
                          criteria,
                          10,
                          cfg->networkId(),
                          cfg->netmask(),
                          addr,
                          addr)
    {
        /* upper addr == lower addr */
    }

    virtual int match(const Client& client) const
    {
        return (m_criteria->check(client) ? 10 : 0);
    }

};

class RootConfigEntity: public NetworkConfigEntity
{
    public:
    RootConfigEntity(std::string name, uint32_t expirationPeriod);
    virtual ~RootConfigEntity(){};
};


#if 0
/**
 * Shared regions e.g. some of configured networks declarations
 * are cover each other.
 * XXX: Shared Network is join on Network config entities with possible
 * overlaps in address pools. for a moment we won't configure and use them them
 */
class SharedNetworkConfigEntity: public NetworkEntity
{
    public:
    SharedNetworkConfigEntity(){}
    int match(const Client& client) const { return m_criteria.match(client)? 3 : 0;}

    SharedNetworkConfigEntity(NetworkEntity& network)
    {
        Networks.push_back(network);
    }
    virtual ~SharedNetworkConfigEntity(){}

    std::vector<NetworkConfigEntity> Networks;

};
#endif

class ConfigurationManager
{
    public:
    static ConfigurationManager* getConfigurationManager();
    static int extractRequestList(PCRTNETBOOTP pDhcpMsg, size_t cbDhcpMsg, RawOption& rawOpt);

    /**
     * 
     */
    Client* getClientByDhcpPacket(const RTNETBOOTP *pDhcpMsg, size_t cbDhcpMsg);

    /**
     * XXX: it's could be done on DHCPOFFER or on DHCPACK (rfc2131 gives freedom here
     * 3.1.2, what is strict that allocation should do address check before real
     * allocation)...
     */
    Lease* allocateLease4Client(Client *client, PCRTNETBOOTP pDhcpMsg, size_t cbDhcpMsg);

    /**
     * We call this before DHCPACK sent and after DHCPREQUEST received ...
     * when requested configuration is acceptable.
     */
    int commitLease4Client(Client *client);
    
    /**
     * Expires client lease.
     */
    int expireLease4Client(Client *client);

    static int findOption(uint8_t uOption, PCRTNETBOOTP pDhcpMsg, size_t cbDhcpMsg, RawOption& opt);

    NetworkConfigEntity *addNetwork(NetworkConfigEntity *pCfg,
                                    const RTNETADDRIPV4& networkId,
                                    const RTNETADDRIPV4& netmask,
                                    RTNETADDRIPV4& UpperAddress,
                                    RTNETADDRIPV4& LowerAddress);

    HostConfigEntity *addHost(NetworkConfigEntity*, const RTNETADDRIPV4&, ClientMatchCriteria*);

    int addToAddressList(uint8_t u8OptId, RTNETADDRIPV4& address)
    {
        switch(u8OptId)
        {
            case RTNET_DHCP_OPT_DNS:
                m_nameservers.push_back(address);
                break;
            case RTNET_DHCP_OPT_ROUTERS:
                m_routers.push_back(address);
                break;
            default:
                Log(("dhcp-opt: list (%d) unsupported\n", u8OptId));
        }
        return VINF_SUCCESS;
    }

    int flushAddressList(uint8_t u8OptId)
    {
       switch(u8OptId)
       {
           case RTNET_DHCP_OPT_DNS:
                m_nameservers.clear();
                break;
           case RTNET_DHCP_OPT_ROUTERS:
               m_routers.clear();
               break;
           default:
               Log(("dhcp-opt: list (%d) unsupported\n", u8OptId));
       }
       return VINF_SUCCESS;
    }

    const Ipv4AddressContainer& getAddressList(uint8_t u8OptId)
    {
       switch(u8OptId)
       {
           case RTNET_DHCP_OPT_DNS:
               return m_nameservers;

           case RTNET_DHCP_OPT_ROUTERS:
               return m_routers;

       }
       /* XXX: Grrr !!! */
       return m_empty;
    }

    private:
    ConfigurationManager(){}
    virtual ~ConfigurationManager(){}

    bool isAddressTaken(const RTNETADDRIPV4& addr, Lease** ppLease = NULL);
    MapLease2Ip4Address m_allocations;
    /**
     * Here we can store expired Leases to do not re-allocate them latter.
     */
    /* XXX: MapLease2Ip4Address m_freed; */
    /*
     *
     */
    Ipv4AddressContainer m_nameservers;
    Ipv4AddressContainer m_routers;
    Ipv4AddressContainer m_empty;
    VecClient m_clients;

};


class NetworkManager
{
    public:
    static NetworkManager *getNetworkManager();

    int offer4Client(Client* lease, uint32_t u32Xid, uint8_t *pu8ReqList, int cReqList);
    int ack(Client *lease, uint32_t u32Xid, uint8_t *pu8ReqList, int cReqList);
    int nak(Client *lease, uint32_t u32Xid);

    const RTNETADDRIPV4& getOurAddress(){ return m_OurAddress;}
    const RTNETADDRIPV4& getOurNetmask(){ return m_OurNetmask;}
    const RTMAC& getOurMac() {return m_OurMac;}

    void setOurAddress(const RTNETADDRIPV4& aAddress){ m_OurAddress = aAddress;}
    void setOurNetmask(const RTNETADDRIPV4& aNetmask){ m_OurNetmask = aNetmask;}
    void setOurMac(const RTMAC& aMac) {m_OurMac = aMac;}

    /* XXX: artifacts should be hidden or removed from here. */
    PSUPDRVSESSION m_pSession;
    INTNETIFHANDLE m_hIf;
    PINTNETBUF m_pIfBuf;

    private:
    int prepareReplyPacket4Client(Client *client, uint32_t u32Xid);
    int doReply(Client *client);
    int processParameterReqList(Client *client, uint8_t *pu8ReqList, int cReqList);

    union {
        RTNETBOOTP BootPHeader;
        uint8_t au8Storage[1024];
    } BootPReplyMsg;
    int cbBooPReplyMsg;

    RTNETADDRIPV4 m_OurAddress;
    RTNETADDRIPV4 m_OurNetmask;
    RTMAC m_OurMac;

    NetworkManager(){}
    virtual ~NetworkManager(){}
};



class Lease
{
public:
    Lease()
    {
        m_address.u = 0;
        m_client = NULL;
        fBinding = false;
        u64TimestampBindingStarted = 0;
        u64TimestampLeasingStarted = 0;
        u32LeaseExpirationPeriod = 0;
        u32BindExpirationPeriod = 0;
        pCfg = NULL;
    }
    virtual ~Lease(){}
    
    bool isExpired()
    {
        if (!fBinding)
            return (ASMDivU64ByU32RetU32(RTTimeMilliTS() - u64TimestampLeasingStarted, 1000) 
                    > u32LeaseExpirationPeriod);
        else
            return (ASMDivU64ByU32RetU32(RTTimeMilliTS() - u64TimestampBindingStarted, 1000) 
                    > u32BindExpirationPeriod);

    }

    /* XXX private: */
    RTNETADDRIPV4 m_address;
    
    /** lease isn't commited */
    bool fBinding;
    
    /** Timestamp when lease commited. */
    uint64_t u64TimestampLeasingStarted;
    /** Period when lease is expired in secs. */
    uint32_t u32LeaseExpirationPeriod;

    /** timestamp when lease was bound */
    uint64_t u64TimestampBindingStarted;
    /* Period when binding is expired in secs. */
    uint32_t u32BindExpirationPeriod;

    NetworkConfigEntity *pCfg;
    Client *m_client;
};





extern const ClientMatchCriteria *g_AnyClient;
extern RootConfigEntity *g_RootConfig;
extern const NullConfigEntity *g_NullConfig;

/**
 * Helper class for stuffing DHCP options into a reply packet.
 */
class VBoxNetDhcpWriteCursor
{
private:
    uint8_t        *m_pbCur;       /**< The current cursor position. */
    uint8_t        *m_pbEnd;       /**< The end the current option space. */
    uint8_t        *m_pfOverload;  /**< Pointer to the flags of the overload option. */
    uint8_t         m_fUsed;       /**< Overload fields that have been used. */
    PRTNETDHCPOPT   m_pOpt;        /**< The current option. */
    PRTNETBOOTP     m_pDhcp;       /**< The DHCP packet. */
    bool            m_fOverflowed; /**< Set if we've overflowed, otherwise false. */

public:
    /** Instantiate an option cursor for the specified DHCP message. */
    VBoxNetDhcpWriteCursor(PRTNETBOOTP pDhcp, size_t cbDhcp) :
        m_pbCur(&pDhcp->bp_vend.Dhcp.dhcp_opts[0]),
        m_pbEnd((uint8_t *)pDhcp + cbDhcp),
        m_pfOverload(NULL),
        m_fUsed(0),
        m_pOpt(NULL),
        m_pDhcp(pDhcp),
        m_fOverflowed(false)
    {
        AssertPtr(pDhcp);
        Assert(cbDhcp > RT_UOFFSETOF(RTNETBOOTP, bp_vend.Dhcp.dhcp_opts[10]));
    }

    /** Destructor.  */
    ~VBoxNetDhcpWriteCursor()
    {
        m_pbCur = m_pbEnd = m_pfOverload = NULL;
        m_pOpt = NULL;
        m_pDhcp = NULL;
    }

    /**
     * Try use the bp_file field.
     * @returns true if not overloaded, false otherwise.
     */
    bool useBpFile(void)
    {
        if (    m_pfOverload
            &&  (*m_pfOverload & 1))
            return false;
        m_fUsed |= 1 /* bp_file flag*/;
        return true;
    }


    /**
     * Try overload more BOOTP fields
     */
    bool overloadMore(void)
    {
        /* switch option area. */
        uint8_t    *pbNew;
        uint8_t    *pbNewEnd;
        uint8_t     fField;
        if (!(m_fUsed & 1))
        {
            fField     = 1;
            pbNew      = &m_pDhcp->bp_file[0];
            pbNewEnd   = &m_pDhcp->bp_file[sizeof(m_pDhcp->bp_file)];
        }
        else if (!(m_fUsed & 2))
        {
            fField     = 2;
            pbNew      = &m_pDhcp->bp_sname[0];
            pbNewEnd   = &m_pDhcp->bp_sname[sizeof(m_pDhcp->bp_sname)];
        }
        else
            return false;

        if (!m_pfOverload)
        {
            /* Add an overload option. */
            *m_pbCur++ = RTNET_DHCP_OPT_OPTION_OVERLOAD;
            *m_pbCur++ = fField;
            m_pfOverload = m_pbCur;
            *m_pbCur++ = 1;     /* bp_file flag */
        }
        else
            *m_pfOverload |= fField;

        /* pad current option field */
        while (m_pbCur != m_pbEnd)
            *m_pbCur++ = RTNET_DHCP_OPT_PAD; /** @todo not sure if this stuff is at all correct... */

        /* switch */
        m_pbCur = pbNew;
        m_pbEnd = pbNewEnd;
        return true;
    }

    /**
     * Begin an option.
     *
     * @returns true on success, false if we're out of space.
     *
     * @param   uOption     The option number.
     * @param   cb          The amount of data.
     */
    bool begin(uint8_t uOption, size_t cb)
    {
        /* Check that the data of the previous option has all been written. */
        Assert(   !m_pOpt
               || (m_pbCur - m_pOpt->dhcp_len == (uint8_t *)(m_pOpt + 1)));
        AssertMsg(cb <= 255, ("%#x\n", cb));

        /* Check if we need to overload more stuff. */
        if ((uintptr_t)(m_pbEnd - m_pbCur) < cb + 2 + (m_pfOverload ? 1 : 3))
        {
            m_pOpt = NULL;
            if (!overloadMore())
            {
                m_fOverflowed = true;
                AssertMsgFailedReturn(("%u %#x\n", uOption, cb), false);
            }
            if ((uintptr_t)(m_pbEnd - m_pbCur) < cb + 2 + 1)
            {
                m_fOverflowed = true;
                AssertMsgFailedReturn(("%u %#x\n", uOption, cb), false);
            }
        }

        /* Emit the option header. */
        m_pOpt = (PRTNETDHCPOPT)m_pbCur;
        m_pOpt->dhcp_opt = uOption;
        m_pOpt->dhcp_len = (uint8_t)cb;
        m_pbCur += 2;
        return true;
    }

    /**
     * Puts option data.
     *
     * @param   pvData      The data.
     * @param   cb          The amount to put.
     */
    void put(void const *pvData, size_t cb)
    {
        Assert(m_pOpt || m_fOverflowed);
        if (RT_LIKELY(m_pOpt))
        {
            Assert((uintptr_t)m_pbCur - (uintptr_t)(m_pOpt + 1) + cb  <= (size_t)m_pOpt->dhcp_len);
            memcpy(m_pbCur, pvData, cb);
            m_pbCur += cb;
        }
    }

    /**
     * Puts an IPv4 Address.
     *
     * @param   IPv4Addr    The address.
     */
    void putIPv4Addr(RTNETADDRIPV4 IPv4Addr)
    {
        put(&IPv4Addr, 4);
    }

    /**
     * Adds an IPv4 address option.
     *
     * @returns true/false just like begin().
     *
     * @param   uOption     The option number.
     * @param   IPv4Addr    The address.
     */
    bool optIPv4Addr(uint8_t uOption, RTNETADDRIPV4 IPv4Addr)
    {
        if (!begin(uOption, 4))
            return false;
        putIPv4Addr(IPv4Addr);
        return true;
    }

    /**
     * Adds an option taking 1 or more IPv4 address.
     *
     * If the vector contains no addresses, the option will not be added.
     *
     * @returns true/false just like begin().
     *
     * @param   uOption     The option number.
     * @param   rIPv4Addrs  Reference to the address vector.
     */
    bool optIPv4Addrs(uint8_t uOption, std::vector<RTNETADDRIPV4> const &rIPv4Addrs)
    {
        size_t const c = rIPv4Addrs.size();
        if (!c)
            return true;

        if (!begin(uOption, 4*c))
            return false;
        for (size_t i = 0; i < c; i++)
            putIPv4Addr(rIPv4Addrs[i]);
        return true;
    }

    /**
     * Puts an 8-bit integer.
     *
     * @param   u8          The integer.
     */
    void putU8(uint8_t u8)
    {
        put(&u8, 1);
    }

    /**
     * Adds an 8-bit integer option.
     *
     * @returns true/false just like begin().
     *
     * @param   uOption     The option number.
     * @param   u8          The integer
     */
    bool optU8(uint8_t uOption, uint8_t u8)
    {
        if (!begin(uOption, 1))
            return false;
        putU8(u8);
        return true;
    }

    /**
     * Puts an 32-bit integer (network endian).
     *
     * @param   u32Network  The integer.
     */
    void putU32(uint32_t u32)
    {
        put(&u32, 4);
    }

    /**
     * Adds an 32-bit integer (network endian) option.
     *
     * @returns true/false just like begin().
     *
     * @param   uOption     The option number.
     * @param   u32Network  The integer.
     */
    bool optU32(uint8_t uOption, uint32_t u32)
    {
        if (!begin(uOption, 4))
            return false;
        putU32(u32);
        return true;
    }

    /**
     * Puts a std::string.
     *
     * @param   rStr        Reference to the string.
     */
    void putStr(std::string const &rStr)
    {
        put(rStr.c_str(), rStr.size());
    }

    /**
     * Adds an std::string option if the string isn't empty.
     *
     * @returns true/false just like begin().
     *
     * @param   uOption     The option number.
     * @param   rStr        Reference to the string.
     */
    bool optStr(uint8_t uOption, std::string const &rStr)
    {
        const size_t cch = rStr.size();
        if (!cch)
            return true;

        if (!begin(uOption, cch))
            return false;
        put(rStr.c_str(), cch);
        return true;
    }

    /**
     * Whether we've overflowed.
     *
     * @returns true on overflow, false otherwise.
     */
    bool hasOverflowed(void) const
    {
        return m_fOverflowed;
    }

    /**
     * Adds the terminating END option.
     *
     * The END will always be added as we're reserving room for it, however, we
     * might have dropped previous options due to overflows and that is what the
     * return status indicates.
     *
     * @returns true on success, false on a (previous) overflow.
     */
    bool optEnd(void)
    {
        Assert((uintptr_t)(m_pbEnd - m_pbCur) < 4096);
        *m_pbCur++ = RTNET_DHCP_OPT_END;
        return !hasOverflowed();
    }
};

#endif
