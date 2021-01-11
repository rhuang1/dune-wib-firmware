#include "wib.h"
#include "unpack.h"
#include "sensors.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <sstream>
#include <unistd.h>
#include <fstream>
#include <sys/mman.h>
#include <fcntl.h>

#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>

#ifdef SIMULATION
#include <cmath>
#endif

using namespace std;

WIB::WIB() {
    io_reg_init(&this->regs,CTRL_REGS,0x10000/4);
    i2c_init(&this->selected_i2c,(char*)"/dev/i2c-0");
    i2c_init(&this->femb_pwr_i2c,(char*)"/dev/i2c-1"); //FIXME these devices appear to be on /dev/i2c-2 ...
    i2c_init(&this->femb_en_i2c,(char*)"/dev/i2c-2");
    #ifdef SIMULATION
    this->daq_spy_fd = -1;
    this->daq_spy[0] = new char[DAQ_SPY_SIZE];
    this->daq_spy[1] = new char[DAQ_SPY_SIZE];
    fake_data((frame14*)this->daq_spy[0],DAQ_SPY_SIZE/sizeof(frame14));
    fake_data((frame14*)this->daq_spy[1],DAQ_SPY_SIZE/sizeof(frame14));
    #else
    this->daq_spy_fd = open("/dev/mem",O_RDWR);
    this->daq_spy[0] = mmap(NULL,DAQ_SPY_SIZE,PROT_READ,MAP_SHARED,this->daq_spy_fd,DAQ_SPY_0);
    this->daq_spy[1] = mmap(NULL,DAQ_SPY_SIZE,PROT_READ,MAP_SHARED,this->daq_spy_fd,DAQ_SPY_1);
    #endif
    for (int i = 0; i < 4; i++) {
        this->femb[i] = new FEMB(i);
    }
}

WIB::~WIB() {
    io_reg_free(&this->regs);
    i2c_free(&this->selected_i2c);
    #ifdef SIMULATION
    delete [] (char*)this->daq_spy[0];
    delete [] (char*)this->daq_spy[1];
    #else
    munmap(this->daq_spy[0],DAQ_SPY_SIZE);
    munmap(this->daq_spy[1],DAQ_SPY_SIZE);
    close(this->daq_spy_fd);
    #endif
    for (int i = 0; i < 4; i++) {
        delete this->femb[i];
    }
}

bool WIB::initialize() {
    bool success = true;
    int ret;
    ret = system("ip link set eth0 up");
    if (WEXITSTATUS(ret) != 0) {
        glog.log("failed to bring up eth0\n");
        success = false;
    }
    string eth0_conf("ip addr add "+crate_ip()+" dev eth0");
    ret = system(eth0_conf.c_str());
    if (WEXITSTATUS(ret) != 0) {
        glog.log("failed to assign IP to eth0\n");
        success = false;
    }
    string route_conf("route add default gw "+gateway_ip()+" eth0");
    ret = system(route_conf.c_str());
    if (WEXITSTATUS(ret) != 0) {
        glog.log("failed to assign default route\n");
        success = false;
    }
    return success;
}

bool WIB::start_frontend() {
    glog.log("Initializing front end...\n");
    bool success = true;
    glog.log("Disabling front end power\n");
    femb_power_set(false);
    glog.log("Configuring front end power\n");
    femb_power_config();
    success &= script("prestart");
    glog.log("Configuring timing endpoint\n");
    success &= timing_endpoint_config();
    glog.log("Resetting FEMB receiver\n");
    femb_rx_mask(0xFFFF); //all disabled
    femb_rx_reset();
    return success;
}

string WIB::crate_ip() {
    glog.log("FIXME: using default IP: 192.168.121.1/24\n");
    return "192.168.121.1/24"; //FIXME pull from firmware
}

string WIB::gateway_ip() {
    glog.log("FIXME: using default IP: 192.168.121.52\n"); //iceberg01
    return "192.168.121.52"; //FIXME pull from somewhere
}

void WIB::set_fake_time(uint64_t time) {
    io_reg_write(&this->regs,REG_FAKE_TIME_L,(uint32_t)(time&0xFFFFFFFF)); //set 4 low bytes
    io_reg_write(&this->regs,REG_FAKE_TIME_H,(uint32_t)((time>>32)&0xFFFFFFFF)); //set 4 high bytes
    io_reg_write(&this->regs,REG_FAKE_TIME_CTRL,0); //disable FTS
}

void WIB::start_fake_time() {
    io_reg_write(&this->regs,REG_FAKE_TIME_CTRL,2); //enable FTS
}

bool WIB::timing_endpoint_config() {
    //timing endpoint reset is bit 28
    io_reg_write(&this->regs,REG_TIMING,1<<28);
    usleep(1000000);
    io_reg_write(&this->regs,REG_TIMING,0);
    usleep(1000000);
    return true;
}

bool WIB::femb_power_ctrl(uint8_t femb_id, uint8_t regulator_id, double voltage) {
    uint8_t chip;
    uint8_t reg;
    uint8_t buffer[2];
    uint32_t DAC_value;

    switch (regulator_id) {
        case 0:
        case 1:
        case 2:
        case 3:
            i2c_select(I2C_PL_FEMB_PWR2);   // SET I2C mux to 0x06 for FEMB DC2DC DAC access
            DAC_value   = (uint32_t) ((voltage * -482.47267) + 2407.15);
            reg         = (uint8_t) (0x10 | ((regulator_id & 0x0f) << 1));
            buffer[0]   = (uint8_t) (DAC_value >> 4) & 0xff;
            buffer[1]   = (uint8_t) (DAC_value << 4) & 0xf0;
            switch(femb_id) {
                case 0:
                    chip = 0x4C;
                    break;  
                case 1:
                    chip = 0x4D;
                    break;  
                case 2:
                    chip = 0x4E;
                    break;  
                case 3:
                    chip = 0x4F;
                    break;  
                default:
                    return false;
            }
            break;
        case 4:
            i2c_select(I2C_PL_FEMB_PWR3);   // SET I2C mux to 0x08 for FEMB LDO DAC access
            chip = 0x4C;
            reg  = (0x10 | ((femb_id & 0x0f) << 1));
            DAC_value   = (uint32_t) ((voltage * -819.9871877) + 2705.465);
            buffer[0]   = (uint8_t) (DAC_value >> 4) & 0xff;
            buffer[1]   = (uint8_t) (DAC_value << 4) & 0xf0;
            break;
        case 5:
            i2c_select(I2C_PL_FEMB_PWR3);   // SET I2C mux to 0x08 for FEMB LDO DAC access
            chip = 0x4D;
            reg  = (0x10 | ((femb_id & 0x0f) << 1));
            DAC_value   = (uint32_t) ((voltage * -819.9871877) + 2705.465);
            buffer[0]   = (uint8_t) (DAC_value >> 4) & 0xff;
            buffer[1]   = (uint8_t) (DAC_value << 4) & 0xf0;
            break;
        default:
            return false;
    }

    i2c_block_write(&this->selected_i2c,chip,reg,buffer,2);

    return true;
}

bool WIB::femb_power_config() {
    for (int i = 0; i <= 3; i++) {
        femb_power_ctrl(i, 0, 4.0);
        femb_power_ctrl(i, 1, 4.0);
        femb_power_ctrl(i, 2, 4.0);
        femb_power_ctrl(i, 3, 4.0);
        femb_power_ctrl(i, 4, 2.5);
        femb_power_ctrl(i, 5, 2.5);
    }
    
    return true;
}

bool WIB::femb_power_set(bool on, bool coldadc) {
    if (on) {
        // configure all pins as outputs
        i2c_reg_write(&this->femb_en_i2c, 0x22, 0xC, 0);
        i2c_reg_write(&this->femb_en_i2c, 0x22, 0xD, 0);
        i2c_reg_write(&this->femb_en_i2c, 0x22, 0xE, 0);
        // set all ones on all outputs
        i2c_reg_write(&this->femb_en_i2c, 0x22, 0x4, coldadc ? 0xFF : 0x6B);
        i2c_reg_write(&this->femb_en_i2c, 0x22, 0x5, coldadc ? 0xFF : 0x6B);
        i2c_reg_write(&this->femb_en_i2c, 0x22, 0x6, coldadc ? 0xFF : 0x6B);
        // configure all pins as outputs
        i2c_reg_write(&this->femb_en_i2c, 0x23, 0xC, 0);
        i2c_reg_write(&this->femb_en_i2c, 0x23, 0xD, 0);
        i2c_reg_write(&this->femb_en_i2c, 0x23, 0xE, 0);
        // set all ones on all outputs
        i2c_reg_write(&this->femb_en_i2c, 0x23, 0x4, coldadc ? 0xFF : 0x6B);
        i2c_reg_write(&this->femb_en_i2c, 0x23, 0x5, coldadc ? 0xFF : 0x6B);
        i2c_reg_write(&this->femb_en_i2c, 0x23, 0x6, coldadc ? 0xFF : 0x6B);
    } else {
        // set all zeros on all outputs
        i2c_reg_write(&this->femb_en_i2c, 0x22, 0x4, 0);
        i2c_reg_write(&this->femb_en_i2c, 0x22, 0x5, 0);
        i2c_reg_write(&this->femb_en_i2c, 0x22, 0x6, 0);
        // set all zeros on all outputs
        i2c_reg_write(&this->femb_en_i2c, 0x23, 0x4, 0);
        i2c_reg_write(&this->femb_en_i2c, 0x23, 0x5, 0);
        i2c_reg_write(&this->femb_en_i2c, 0x23, 0x6, 0);
    }
    return true;
}

bool WIB::femb_rx_mask(uint32_t value, uint32_t mask) {
    uint32_t prev = io_reg_read(&this->regs,REG_LINK_MASK);
    value = (prev & (~mask)) | (value & mask);
    io_reg_write(&this->regs,REG_LINK_MASK,value);
    return true;
}

bool WIB::femb_rx_reset() {
    //rx_reset is bit 13
    uint32_t value = io_reg_read(&this->regs,REG_FW_CTRL);
    value |= (1<<13);
    io_reg_write(&this->regs,REG_FW_CTRL,value);
    value &= ~(1<<13);
    io_reg_write(&this->regs,REG_FW_CTRL,value);
    return true;
}

bool WIB::script_cmd(string line) {
    istringstream ss(line);
    istream_iterator<string> begin(ss);
    istream_iterator<string> end;
    vector<string> tokens(begin, end);
    if (tokens.size() == 0 || tokens[0][0] == '#') return true;
    string cmd(tokens[0]);
    if (cmd == "delay") {
        if (tokens.size() != 2) {
            glog.log("Invalid delay\n");
            return false;
        }
        size_t micros = (size_t) strtoull(tokens[1].c_str(),NULL,10);
        usleep(micros);
        return true;
    } else if (cmd == "run") {
        if (tokens.size() != 2) {
            glog.log("Invalid run\n");
            return false;
        }
        return script(tokens[1]);
    } else if (cmd == "i2c") {
        string bus(tokens[1]);
        if (bus == "cd") { // i2c cd femb coldata chip page addr data
            uint8_t femb_idx = (uint8_t)strtoull(tokens[2].c_str(),NULL,10);
            uint8_t coldata_idx = (uint8_t)strtoull(tokens[3].c_str(),NULL,10);
            uint8_t chip_addr = (uint8_t)strtoull(tokens[4].c_str(),NULL,16);
            uint8_t reg_page = (uint8_t)strtoull(tokens[5].c_str(),NULL,16);
            uint8_t reg_addr = (uint8_t)strtoull(tokens[6].c_str(),NULL,16);
            uint8_t data = (uint8_t)strtoull(tokens[7].c_str(),NULL,16);
            cdpoke(femb_idx, coldata_idx, chip_addr, reg_page, reg_addr, data);
            return true;
        } else {
            i2c_t *i2c_bus;
            if (bus == "sel") {  // i2c sel chip addr data [...]
                i2c_bus = &this->selected_i2c;
            } else if (bus == "pwr") { // i2c pwr chip addr data [...]
                i2c_bus = &this->femb_en_i2c;
            } else {
                glog.log("Invalid i2c bus selection: %s\n", tokens[1].c_str());
                return false;
            }
            uint8_t chip = (uint8_t)strtoull(tokens[2].c_str(),NULL,16);
            uint8_t addr = (uint8_t)strtoull(tokens[3].c_str(),NULL,16);
            if (tokens.size() < 5) {
                glog.log("Invalid arguments to i2c\n");
            } else if (tokens.size() > 5) {
                size_t size = tokens.size() - 4;
                uint8_t *buf = new uint8_t[size];
                for (size_t i = 0; i < size; i++) {
                    buf[i] = (uint8_t)strtoull(tokens[4+i].c_str(),NULL,16);
                }
                i2c_block_write(i2c_bus, chip, addr, buf, size);
                delete [] buf;
                return true;
            } else {
                uint8_t data = (uint8_t)strtoull(tokens[4].c_str(),NULL,16);
                i2c_reg_write(i2c_bus, chip, addr, data);
                return true;
            }
        }
    } else if (cmd == "mem") {
        if (tokens.size() == 3) { // mem addr value
            uint32_t addr = strtoull(tokens[1].c_str(),NULL,16);
            uint32_t value = strtoull(tokens[2].c_str(),NULL,16);
            poke(addr, value);
            return true;
        } else if (tokens.size() == 4) { // mem addr value mask
            uint32_t addr = strtoull(tokens[1].c_str(),NULL,16);
            uint32_t value = strtoull(tokens[2].c_str(),NULL,16);
            uint32_t mask = strtoull(tokens[3].c_str(),NULL,16);
            uint32_t prev = peek(addr);
            poke(addr, (prev & (~mask)) | (value & mask));
            return true;
        } else {
            glog.log("Invalid arguments to mem\n");
        }
    } else if (cmd == "fast") {
        if (tokens.size() != 2) {
            glog.log("Invalid arguments to fast\n");
            return false;
        }
        string fast(tokens[1]);
        if (fast == "reset") {
            FEMB::fast_cmd(FAST_CMD_RESET);
        } else if (cmd == "act") {
            FEMB::fast_cmd(FAST_CMD_ACT);
        } else if (fast == "sync") {
            FEMB::fast_cmd(FAST_CMD_SYNC);
        } else if (fast == "edge") {
            FEMB::fast_cmd(FAST_CMD_EDGE);
        } else if (fast == "idle") {
            FEMB::fast_cmd(FAST_CMD_IDLE);
        } else if (fast == "edge_act") {
            FEMB::fast_cmd(FAST_CMD_EDGE_ACT);
        } else {
            glog.log("Unknown fast command: %s\n",fast.c_str());
            return false;
        }
        return true;
    } else {
        glog.log("Invalid script command: %s\n", tokens[0].c_str());
    }
    return false;
}

bool WIB::script(string script, bool file) {
    if (file) {
        ifstream fin(script);
        if (!fin.is_open()) {
            fin.clear();
            fin.open("scripts/"+script);
            if (!fin.is_open()) {
                fin.clear();
                fin.open("/etc/wib/"+script);        
                if (!fin.is_open()) {
                    return false;
                } else {
                    glog.log("Found /etc/wib/%s on WIB\n",script.c_str());
                }
            } else {
                glog.log("Found scripts/%s on WIB\n",script.c_str());
            }
        } else {
            glog.log("Found full or relative path %s on WIB\n",script.c_str());
        }
        glog.log("Running script: %s\n",script.c_str());
        string str((istreambuf_iterator<char>(fin)), istreambuf_iterator<char>());
        fin.close();
        script = str;
    } else {
        glog.log("Running remote/generated script\n");
    }
    istringstream iss(script);

    for (string line; getline(iss, line); ) {
        //glog.log("%s\n",line.c_str());
        if (!script_cmd(line)) return false;
    }
    return true;
}

void WIB::i2c_select(uint8_t device) {
    uint32_t next = io_reg_read(&this->regs,REG_FW_CTRL);
    next = (next & 0xFFFFFFF0) | (device & 0xF);
    io_reg_write(&this->regs,REG_FW_CTRL,next);
}

bool WIB::read_daq_spy(void *buf0, void *buf1) {
    uint32_t prev = io_reg_read(&this->regs,REG_FW_CTRL);
    uint32_t mask = 0;
    if (buf0) mask |= (1 << 0);
    if (buf1) mask |= (1 << 1);
    //acquisition start are bits 6 and 7 (one per buffer)
    prev &= (~(mask << 6));
    uint32_t next = prev | (mask << 6);
    glog.log("Starting acquisition...\n");
    io_reg_write(&this->regs,REG_FW_CTRL,next);
    io_reg_write(&this->regs,REG_FW_CTRL,prev);
    bool success = false;
    uint32_t last_read;
    int ms;
    for (ms = 0; ms < 100; ms++) { // try for 100 ms (should take max 4)
        usleep(1000);
        if (((last_read = io_reg_read(&this->regs,REG_DAQ_SPY_STATUS)) & mask) == mask) {
            success = true;
            break;
        }
    }
    if (!success) {
        glog.log("Timed out waiting for buffers to fill: %0X\n",last_read);
    } else {
        glog.log("Acquisition took %i ms\n",ms);
    }
    glog.log("Copying spydaq buffers\n");
    if (buf0) memcpy(buf0,this->daq_spy[0],DAQ_SPY_SIZE);
    if (buf1) memcpy(buf1,this->daq_spy[1],DAQ_SPY_SIZE);
    #ifdef SIMULATION
    //generate more "random" data for simulation
    glog.log("Generating random sin/cos data for next acquisiton\n");
    fake_data((frame14*)this->daq_spy[0],DAQ_SPY_SIZE/sizeof(frame14));
    fake_data((frame14*)this->daq_spy[1],DAQ_SPY_SIZE/sizeof(frame14));
    #endif
    return success;
}

uint32_t WIB::peek(size_t addr) {
    #ifndef SIMULATION
    size_t page_addr = (addr & ~(sysconf(_SC_PAGESIZE)-1));
    size_t page_offset = addr-page_addr;

    int fd = open("/dev/mem",O_RDWR);
    void *ptr = mmap(NULL,sysconf(_SC_PAGESIZE),PROT_READ|PROT_WRITE,MAP_SHARED,fd,(addr & ~(sysconf(_SC_PAGESIZE)-1)));

    return *((uint32_t*)((char*)ptr+page_offset));
    
    munmap(ptr,sysconf(_SC_PAGESIZE));
    close(fd);
    #else
    return 0x0;
    #endif
}

void WIB::poke(size_t addr, uint32_t val) {
    #ifndef SIMULATION
    size_t page_addr = (addr & ~(sysconf(_SC_PAGESIZE)-1));
    size_t page_offset = addr-page_addr;

    int fd = open("/dev/mem",O_RDWR);
    void *ptr = mmap(NULL,sysconf(_SC_PAGESIZE),PROT_READ|PROT_WRITE,MAP_SHARED,fd,(addr & ~(sysconf(_SC_PAGESIZE)-1)));

    *((uint32_t*)((char*)ptr+page_offset)) = val;
    
    munmap(ptr,sysconf(_SC_PAGESIZE));
    close(fd);
    #endif
}

uint8_t WIB::cdpeek(uint8_t femb_idx, uint8_t coldata_idx, uint8_t chip_addr, uint8_t reg_page, uint8_t reg_addr) {
    return this->femb[femb_idx]->i2c_read(coldata_idx,chip_addr,reg_page,reg_addr);
}

void WIB::cdpoke(uint8_t femb_idx, uint8_t coldata_idx, uint8_t chip_addr, uint8_t reg_page, uint8_t reg_addr, uint8_t data) {
    this->femb[femb_idx]->i2c_write(coldata_idx,chip_addr,reg_page,reg_addr,data);
}

bool WIB::reboot() {
    int ret = system("reboot");
    return WEXITSTATUS(ret) == 0;
}

bool WIB::update(const string &root_archive, const string &boot_archive) {
    ofstream out_boot("/home/root/boot_archive.tar.gz", ofstream::binary);
    out_boot.write(boot_archive.data(),boot_archive.size());
    out_boot.close();
    glog.log("Expanding boot archive (%0.1f MB)\n",boot_archive.size()/1024.0/1024.0);
    int ret1 = system("wib_update.sh /home/root/boot_archive.tar.gz /boot");
    
    ofstream out_root("/home/root/root_archive.tar.gz", ofstream::binary);
    out_root.write(root_archive.data(),root_archive.size());
    out_root.close();
    glog.log("Expanding root archive (%0.1f MB)\n",root_archive.size()/1024.0/1024.0);
    int ret2 = system("wib_update.sh /home/root/root_archive.tar.gz /");
    
    return WEXITSTATUS(ret1) == 0 && WEXITSTATUS(ret2) == 0;
}


bool WIB::configure_wib(wib::ConfigureWIB &conf) {

    if (!frontend_initialized) {
        if (!start_frontend()) {
            glog.log("Failed to start frontend electronics\n");
            return false;
        }
        frontend_initialized = true;
    }
    
    if (conf.fembs_size() != 4) {
        glog.log("Must supply exactly 4 FEMB configurations\n");
        return false;
    }
    
    glog.log("Reconfiguring WIB\n"); 
    
    glog.log("Powering on COLDATA\n");
    femb_power_set(true,false); // COLDATA on, COLDADC off
    usleep(1000000);
    glog.log("Resetting COLDATA\n");
    FEMB::fast_cmd(FAST_CMD_RESET); // Reset COLDATA
    bool coldata_res = true;
    for (int i = 0; i < 4; i++) { // Configure COLDATA
        if (conf.fembs(i).enabled()) coldata_res &= femb[i]->configure_coldata(conf.cold(),FRAME_14); // Sets ACT to ACT_RESET_COLDADC
    }
    if (coldata_res) {
        glog.log("COLDATA configured\n");
    } else {
        glog.log("COLDATA configuration failed!\n");
    }
    
    glog.log("Powering on COLDADC\n");
    femb_power_set(true,true); // COLDATA on, COLDADC on
    usleep(1000000);
    FEMB::fast_cmd(FAST_CMD_EDGE_ACT); // Perform ACT
    bool coldadc_res = true;
    for (int i = 0; i < 4; i++) { // Configure COLDADCs
         if (conf.fembs(i).enabled()) coldadc_res &= femb[i]->configure_coldadc();
    }
    if (coldadc_res) {
        glog.log("COLDADC configured\n");
    } else {
        glog.log("COLDADC configuration failed!\n");
    }
    
    glog.log("Powering on VDDA and VDDD L/R\n");
    bool power_res = true;
    for (int i = 0; i < 4; i++) {
        if (conf.fembs(i).enabled()) {
            power_res &= femb[i]->set_control_reg(0,true,true); //VDDA on U1 ctrl_1
            power_res &= femb[i]->set_control_reg(1,true,true);  //VDDD L/R on U2 ctrl_0/ctrl_1
        }
    }
    if (power_res) {
        glog.log("VDDA and VDDD L/R powered succesfully\n");
    } else {
        glog.log("VDDA and VDDD L/R power failed!\n");
    }
    
    bool larasic_res = true;
    uint32_t rx_mask = 0x0000;
    for (int i = 0; i < 4; i++) {
        if (conf.fembs(i).enabled()) {
            larasic_conf c;
            memset(&c,0,sizeof(larasic_conf));
            
            const wib::ConfigureWIB::ConfigureFEMB &femb_conf = conf.fembs(i);
            
            c.sdd = femb_conf.buffer() == 2;
            c.sdc = femb_conf.ac_couple() == true;
            c.slkh = femb_conf.leak_10x() == true;
            c.slk = femb_conf.leak() == 1;
            c.sdac = femb_conf.pulse_dac() & 0x3F;
            c.sdacsw2 = conf.pulser(); //connect pulser to channels
            
            c.sts = femb_conf.test_cap() == true;
            c.snc = femb_conf.baseline() == 1;
            c.gain = femb_conf.gain() & 0x3;
            c.peak_time = femb_conf.peak_time() & 0x3;
            c.sdf = femb_conf.buffer() == 1;    
            
            c.cal_skip = femb_conf.strobe_skip();
            c.cal_delay = femb_conf.strobe_delay();
            c.cal_length = femb_conf.strobe_length();    
            
            larasic_res &= femb[i]->configure_larasic(c); // Sets ACT to ACT_PROGRAM_LARASIC
        } else {
            rx_mask |= (0xF << (i*4));
        }
    }
    FEMB::fast_cmd(FAST_CMD_EDGE_ACT); // Perform ACT
    if (larasic_res) {
        glog.log("LArASIC configured\n");
    } else {
        glog.log("LArASIC configuration failed!\n");
    }
    
    bool spi_verified = false;
    for (int i = 0; i < 10; i++) {
        usleep(10000);
        bool verify_res = true;
        for (int i = 0; i < 4; i++) {
            if (conf.fembs(i).enabled()) {
                verify_res &= femb[i]->set_fast_act(ACT_SAVE_STATUS);
            }
        }
        if (!verify_res) continue;
        FEMB::fast_cmd(FAST_CMD_EDGE_ACT); // Perform ACT
        for (int i = 0; i < 4; i++) {
            if (conf.fembs(i).enabled()) {
                verify_res &= femb[i]->read_spi_status();
            }
        }
        if (verify_res) {
            spi_verified = true;
            break;
        }
    }
    if (spi_verified) {
        glog.log("LArASIC SPI verified\n");
    } else {
        glog.log("LArASIC SPI verification failed!\n");
    }
    
    bool pulser_res = true;
    if (conf.pulser()) {
        for (int i = 0; i < 4; i++) { 
            if (conf.fembs(i).enabled()) {
                pulser_res &= femb[i]->set_fast_act(ACT_LARASIC_PULSE);
            }
        }
        FEMB::fast_cmd(FAST_CMD_EDGE_ACT); // Perform ACT
        if (pulser_res) {
            glog.log("Calibration pulser started\n");
        } else {
            glog.log("Calibration pulser start failed!\n");
        }
    }
        
    femb_rx_mask(rx_mask); 
    femb_rx_reset();
    glog.log("Serial receivers reset\n");
    
    return coldata_res && coldadc_res && power_res && larasic_res && spi_verified;
}

bool WIB::read_sensors(wib::GetSensors::Sensors &sensors) {
   
    glog.log("Activating I2C_SENSOR bus\n");
    i2c_select(I2C_SENSOR);

    glog.log("Enabling voltage sensors\n");
    uint8_t buf[1] = {0x7};
    i2c_write(&this->selected_i2c,0x70,buf,1); // enable i2c repeater
    
    // 5V (before)
    // 5V
    // VCCPSPLL_Z_1P2V
    // PS_DDR4_VTT
    enable_ltc2990(&this->selected_i2c,0x4E);
    sensors.clear_ltc2990_4e_voltages();
    for (uint8_t i = 1; i <= 4; i++) {
        double v = 0.00030518*read_ltc2990_value(&this->selected_i2c,0x4E,i);
        glog.log("LTC2990 0x4E ch%i -> %0.2f V\n",i,v);
        sensors.add_ltc2990_4e_voltages(v);
    }
    glog.log("LTC2990 0x4E Vcc -> %0.2f V\n",0.00030518*read_ltc2990_value(&this->selected_i2c,0x4E,6)+2.5);

    // 1.2 V (before)
    // 1.2 V
    // 3.3 V (before)
    // 3.3 V
    enable_ltc2990(&this->selected_i2c,0x4C);
    sensors.clear_ltc2990_4c_voltages();
    for (uint8_t i = 1; i <= 4; i++) {
        double v = 0.00030518*read_ltc2990_value(&this->selected_i2c,0x4C,i);
        glog.log("LTC2990 0x4C ch%i -> %0.2f V\n",i,v);
        sensors.add_ltc2990_4c_voltages(v);
    }
    glog.log("LTC2990 0x4C Vcc -> %0.2f V\n",0.00030518*read_ltc2990_value(&this->selected_i2c,0x4C,6)+2.5);

    // In pairs (before,after)
    // 0.85 V
    // 0.9 V
    // 2.5 V
    // 1.8 V
    enable_ltc2991(&this->selected_i2c,0x48);
    sensors.clear_ltc2991_48_voltages();
    for (uint8_t i = 1; i <= 8; i++) {
        double v = 0.00030518*read_ltc2991_value(&this->selected_i2c,0x48,i);
        glog.log("LTC2991 0x48 ch%i -> %0.2f V\n",i,v);
        sensors.add_ltc2991_48_voltages(v);
    }
    glog.log("LTC2991 0x48 Vcc -> %0.2f V\n",0.00030518*read_ltc2991_value(&this->selected_i2c,0x48,10)+2.5);

    // 0x49 0x4D 0x4A are AD7414 temperature sensors
    double t;
    t = read_ad7414_temp(&this->selected_i2c,0x49);
    glog.log("AD7414 0x49 temp %0.1f\n", t);
    sensors.set_ad7414_49_temp(t);
    t = read_ad7414_temp(&this->selected_i2c,0x4D);
    glog.log("AD7414 0x4D temp %0.1f\n", t);
    sensors.set_ad7414_4d_temp(t);
    t = read_ad7414_temp(&this->selected_i2c,0x4A);
    glog.log("AD7414 0x4A temp %0.1f\n", t);
    sensors.set_ad7414_4a_temp(t);

    // 0x15 LTC2499 temperature sensor inputs from LTM4644 for FEMB 0 - 3 and WIB 1 - 3
    start_ltc2499_temp(&this->selected_i2c,0);
    sensors.clear_ltc2499_15_temps();
    for (uint8_t i = 0; i < 7; i++) {
        usleep(175000);
        t = read_ltc2499_temp(&this->selected_i2c,i+1);
        glog.log("LTC2499 ch%i -> %0.14f\n",i,t);
        sensors.add_ltc2499_15_temps(t);
    }

    // FIXME 0x46 an INA226 for DDR current
    
    //FEMB power monitoring
    //docs suggest these should be on the selected_i2c bus set to 3, but they aren't
    i2c_t *femb_power_mon_i2c = &this->femb_en_i2c; 
    
    uint8_t femb_dc2dc_current_addr[4] = {0x48,0x49,0x4a,0x4b};  //DC2DC 0-3 in pairs for FEMBs 0-3
    uint8_t femb_ldo_current_addr[2] = {0x4c,0x4d}; //LDO femb 0-3 in pairs for LDO 0-1
    uint8_t femb_bias_current_addr[1] = {0x4e}; //BIAS femb 0-3 in pairs 
    
    sensors.clear_femb0_dc2dc_ltc2991_voltages();
    sensors.clear_femb1_dc2dc_ltc2991_voltages();
    sensors.clear_femb2_dc2dc_ltc2991_voltages();
    sensors.clear_femb3_dc2dc_ltc2991_voltages();
    sensors.clear_femb_ldo_a0_ltc2991_voltages();
    sensors.clear_femb_ldo_a0_ltc2991_voltages();
    sensors.clear_femb_bias_ltc2991_voltages();
    
    for (uint8_t i = 0; ; i++) {
        uint8_t addr;
        if (i < 4) {
            glog.log("Reading FEMB%i DC2DC current sensor\n",i);
            addr = femb_dc2dc_current_addr[i];
        } else if (i < 6) {
            glog.log("Reading FEMB LDO %i current\n",i-4);
            addr = femb_ldo_current_addr[i-4];
        } else if (i < 7) {
            glog.log("Reading FEMB bias current\n");
            addr = femb_bias_current_addr[i-6];
        } else {
            break;
        }
        enable_ltc2991(femb_power_mon_i2c,addr);
        for (uint8_t j = 1; j <= 8; j++) {
            double v = 0.00030518*read_ltc2991_value(femb_power_mon_i2c,addr,j);
            glog.log("LTC2991 0x%X ch%i -> %0.2f V\n",addr,j,v);
            switch (i) {
                case 0: sensors.add_femb0_dc2dc_ltc2991_voltages(v); break;
                case 1: sensors.add_femb1_dc2dc_ltc2991_voltages(v); break;
                case 2: sensors.add_femb2_dc2dc_ltc2991_voltages(v); break;
                case 3: sensors.add_femb3_dc2dc_ltc2991_voltages(v); break;
                case 4: sensors.add_femb_ldo_a0_ltc2991_voltages(v); break;
                case 5: sensors.add_femb_ldo_a1_ltc2991_voltages(v); break;
                case 6: sensors.add_femb_bias_ltc2991_voltages(v); break;
            }   
        }
        glog.log("LTC2991 0x%X Vcc -> %0.2f V\n",addr,0.00030518*read_ltc2991_value(femb_power_mon_i2c,addr,10)+2.5);
    }

    return true;
}

uint32_t WIB::read_fw_timestamp() {
    return io_reg_read(&this->regs,REG_FW_TIMESTAMP);
}

