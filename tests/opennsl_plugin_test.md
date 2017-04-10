# OpenNSL Plugin Test Cases

## Contents
- [Test loopback creation and deletion](#test-loopback-creation-and-deletion)
- [ECMP resilient test cases](#ecmp-resilient-test-cases)
- [Test TCP/UDP load balance mode](#test-tcp/udp-load-balance-mode)
- [Test L3 LAG creation and deletion](#test-l3-lag-creation-and-deletion)
- [Test OSPF field processor entries](#test-ospf-field-processor-entries)
- [Test sFlow](#test-sflow)
- [Test L3 ipv4/ipv6 statistics collected from the ASIC ](#test-l3-fp-statistics)

## Test loopback creation and deletion
### Objective
Verify creating a loopback interface, and assigning an IP address to it. Also verify deleting the loopback interface.
### Requirements
 - RTL setup with physical switch

### Setup
#### Topology diagram
```
[switch1] <==> [host1]
```
### Description
1. Create port 1 on switch1.
2. Assign the IP address 10.0.10.1 to port 1.
3. Get the UUID of the port 1.
4. Using the `ovsdb-client` command:
 - Create a loopback interface lo:1 of type loopback.
 - Create a port lo:1 and assign the interface lo:1 to it.
 - Assign the port lo:1 to vrf_default along with port 1.
5. Assign the IP address 2.2.2.1 to port lo:1.
6. Configure host1 eth1 with the IP address 10.0.10.2 and default gateway 10.0.10.1.
7. Ping 2.2.2.1 from host1.
8. Using `ovsdb-client`, delete port lo:1.
9. Ping 2.2.2.1 from host1.

### Test result criteria
#### Test pass criteria
The first ping passes and the second ping fails.
#### Test fail criteria
The first ping fails and the second ping passes.

## ECMP resilient test cases
### Objective
The ECMP resiliency is toggled and all the l3 ecmp egress objects reflect the appropriate state.

### Requirements
 - RTL setup with physical switch

### Setup
#### Topology diagram
```
[switch1] <==> [switch2]
```

### Description
1. Configure three interfaces between switch1 and switch2 with IP addresses.
2. Configure static routes with three different nexthops. For example, the 3 links between switch1 and switch2.
3. Check for the `ovs-appctl` command to see if ECMP resiliency is set.
4. Disable the ECMP resiliency, and check the `ovs-appctl` command to see if ECMP resiliency is unset.

### Test result criteria
#### Test pass criteria
 -  If ECMP resiliency is enabled for all l3ecmp objects in default state.
 -  If ECMP resiliency is disabled when disabled through configuration.
 -  Dynamic Size is 512 when resiliency is enabled and 0 when disabled.
 -  All l3 ecmp egress objects should adhere to the above criteria.

#### Test fail criteria
When the resiliency flag in the l3 ecmp egress object is false when enabled, or set to true when disabled.

## Test TCP/UDP load balance mode
### Objective
This test checks for the load balance mode programmed in the ASIC.
### Requirements
 - Two physical switches.

### Setup
#### Topology diagram
```ditaa
+---------------+            +---------------+
|               |            |               |
|               +------------+               |
|   Switch 1    |            |   Switch 2    |
|               +------------+               |
|               |            |               |
+---------------+            +---------------+
```
### Description
1. Create interface lag 100 on switch 1.
2. Select hashing algorithm hash l4-src-dst on switch 1.
3. Add interface 2 to lag 100 on switch 1.
4. Add interface 3 to lag 100 on switch 1.
5. Create interface lag 100 on switch 2.
6. Select hashing algorithm hash l4-src-dst on switch 2.
7. Add interface 2 to lag 100 on switch 2.
8. Add interface 3 to lag 100 on switch 2.
9. Use the `ovs-appctl` command on switch 1 for retrieving the load mode entry in the ASIC.
    ```
    ovs-appctl plugin/debug lag
    ```

### Test result criteria
#### Test pass criteria
The load mode entry in the ASIC is l4-src-dst.
#### Test fail criteria
The load mode entry in the ASIC is not l4-src-dst.

## Test L3 LAG creation and deletion
### Objective
Verify LAG L3 interface add/deletes members, add/deletes knet filters, and creates LAG in the hardware.

### Requirements
 - RTL setup with physical switch

### Setup
#### Topology diagram
```
[switch1] <==> [switch2]
```

### Description
1. Enable two interfaces between switch1 and switch2.
2. Configure L3 LAG and add these interfaces as members.
3. Using `ovs-appctl` test if:
    a. the internal VLAN is created.
    b. the internal VLAN has the members in its bitmap.
    c. the lag has the members in its bitmap.
4. Shutdown interface 1, and repeat test in step 3.
5. Shutdown interface 2, and repeat test in step 3.
6. Enable both interfaces.
7. Remove interface 1 from LAG, and repeat test in step 3.
8. Remove interface 2 from LAG, and repeat test in step 3.

### Test result criteria
#### Test pass criteria
   Internal VLAN should be created with members in the bitmap.
   LAG should be created with members in the bitmap.
   When both interfaces are 'shutdown' the lag should exist but the bitmap should be all zeros.
   When both interfaces are 'shutdown' the VLAN should be deleted.
   When both interfaces are removed from the lag, LAG should be destroyed.
   When both interfaces are removed from LAG the VLAN should be deleted.

#### Test fail criteria
   Internal VLAN does not exists with members in the bitmap after LAG creation.
   LAG not created with members in the bitmap.
   When both interfaces are 'shutdown' the lag does not exist or the bitmap show non-zero.
   When both interfaces are 'shutdown' the VLAN does not get deleted.
   When both interfaces are removed from the lag, LAG exists.
   When both interfaces are removed from LAG the VLAN does not get deleted.

## Test OSPF field processor entries
### Objective
This test checks for the two OSPF field processor entries programmed in the ASIC.
### Requirements
A physical switch is required for this test.

### Setup
#### Topology diagram
```ditaa
+---------------+
|               |
|    Switch     |
|               |
+---------------+
```
### Description
1. Use the `ovs-appctl` command for retrieving the existing field processor (FP) entries in the ASIC.
    ```
    ovs-appctl plugin/debug fp
    ```

2. Check for the FP entry that forwards OSPF "All Routers" traffic to the CPU. The entry has the following qualifiers:
    - Destination MAC address - 01:00:5E:00:00:05
    - Destination IP address -  224.0.0.5
    - Protocol Type - 0x59
3. Check for the FP entry that forwards OSPF "Designated Routers" traffic to the CPU. The entry has the following qualifiers:
    - Destination MAC address - 01:00:5E:00:00:06
    - Destination IP address -  224.0.0.6
    - Protocol Type - 0x59

### Test result criteria
#### Test pass criteria
The two OSPF field processor entries are present in the ASIC.
#### Test fail criteria
None of the OSPF field processor entries are present in the ASIC.

## Test sFlow
### Objective
The test case checks whether sFlow (Sampled Flow) gets successfully configured in the ASIC through OpenSwitch CLI.
We verify both the ingress and egress sampling rates and the KNET filter ids created each for source and destination samples.
### Requirements
A physical switch is required for this test.

### Setup
#### Topology diagram
```ditaa
+---------------+
|               |
|    Switch     |
|               |
+---------------+
```
### Description\
1. Enable sFlow and configure sampling rate and collector IP as shown below
```
switch(config)# sflow enable
switch(config)# sflow sampling 100
switch(config)# sflow collector 10.10.10.2
```
2. Use the `ovs-appctl` command for retrieving the ingress and the egress sampling rates set in the ASIC.
```
ovs-appctl -t ops-switchd sflow/show-rate
```
3. Use the `ovs-appctl` command for retrieving the KNET filter ids created in the ASIC for source and destination samples.
```
ovs-appctl plugin/debug knet filter
```
### Test result criteria
#### Test pass criteria
The sFlow sampling rate should to be set on all the displayed ports (for eg. 1 - 72)
KNET filters should be created for source and destination samples

#### Test fail criteria
Sampling rate is not configured in ASIC or creation of KNET filters for sFlow failed

## Test L3 ipv4/ipv6 statistics collected from the ASIC.
### Objective
Verify the statistic counters for L3 IPv4 and IPv6 traffic collected from the ASIC.
### Requirements
RTL setup with physical switch

### Setup
#### Topology diagram
```
[host1] <==> [switch1] <==> [host2]
```
### Description
1. Enable interface 1 on switch1.
2. Assign IPv4 address 2.2.2.1/24 and IPv6 address 1000::1/120 on interface 1.
3. Enable interface 2 on switch1.
4. Assign IPv4 address 3.3.3.1/24 and IPv6 address 2000::1/120 on interface 2.
5. Configure host1 eth1 with IPv4 address 2.2.2.2 and default gateway 2.2.2.1.
6. Configure host1 eth1 with IPv6 address 1000::2/120 and default gateway 1000::1/120.
7. Configure host2 eth1 with IPv4 address 3.3.3.2 and default gateway 3.3.3.1.
8. Configure host2 eth1 with IPv6 address 2000::2/120 and default gateway 2000::1/120.
9. Ping host2 from host1 using IPv4 and IPv6 protocol types.
10. Using the 'ovs-vsctl list interface' command, verify that the Rx/Tx counters for IPv4 and IPv6 traffic increase correctly on interface 1 and 2 of the switch.

### Test result criteria
#### Test pass criteria
IPv4 and IPv6 pings are successful, and the RX/TX counters for IPv4 and IPv6 traffic correctly increase.
#### Test fail criteria
Either one of the pings fail, or the RX/TX counters for IPv4 and IPv6 traffic do not correctly increase.
