/*
 *    Copyright (C) 2018
 *    Matthias P. Braendli (matthias.braendli@mpb.li)
 *
 *    Copyright (C) 2017
 *    Albrecht Lohofener (albrechtloh@gmx.de)
 *
 *    This file is based on SDR-J
 *    Copyright (C) 2010, 2011, 2012
 *    Jan van Katwijk (J.vanKatwijk@gmail.com)
 *
 *    This file is part of the welle.io.
 *    Many of the ideas as implemented in welle.io are derived from
 *    other work, made available through the GNU general Public License.
 *    All copyrights of the original authors are recognized.
 *
 *    welle.io is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    welle.io is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with welle.io; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <utility>
#include <cstdio>
#include <unistd.h>
#ifdef HAVE_SOAPYSDR
#  include "CSoapySdr.h"
#endif
#if defined(HAVE_ALSA)
#  include "welle-cli/alsa-output.h"
#endif
#include "welle-cli/webradiointerface.h"
#include "welle-cli/tests.h"
#include "backend/radio-receiver.h"
#include "input/CInputFactory.h"
#include "input/CRAWFile.h"
#include "various/channels.h"
#include "libs/json.hpp"
extern "C" {
#include "various/wavfile.h"
}

using namespace std;

using namespace nlohmann;

#if defined(HAVE_ALSA)
class AlsaProgrammeHandler: public ProgrammeHandlerInterface {
    public:
        virtual void onFrameErrors(int frameErrors) override { (void)frameErrors; }
        virtual void onNewAudio(std::vector<int16_t>&& audioData, int sampleRate, bool isStereo, const std::string& mode) override
        {
            (void)mode;
            lock_guard<mutex> lock(aomutex);

            bool reset_ao = (sampleRate != (int)rate) or (isStereo != stereo);
            rate = sampleRate;
            stereo = isStereo;

            if (!ao or reset_ao) {
                cerr << "Create audio output with stereo " << stereo << " and rate " << rate << endl;
                ao = make_unique<AlsaOutput>(stereo ? 2 : 1, rate);
            }

            ao->playPCM(move(audioData));
        }

        virtual void onRsErrors(int rsErrors) override { (void)rsErrors; }
        virtual void onAacErrors(int aacErrors) override { (void)aacErrors; }
        virtual void onNewDynamicLabel(const std::string& label) override
        {
            cout << "DLS: " << label << endl;
        }

        virtual void onMOT(const std::vector<uint8_t>& data, int subtype) override { (void)data; (void)subtype; }

    private:
        mutex aomutex;
        unique_ptr<AlsaOutput> ao;
        bool stereo = true;
        unsigned int rate = 48000;
};
#endif // defined(HAVE_ALSA)

class WavProgrammeHandler: public ProgrammeHandlerInterface {
    public:
        WavProgrammeHandler(uint32_t SId, const std::string& fileprefix) :
            SId(SId),
            filePrefix(fileprefix) {}
        ~WavProgrammeHandler() {
            if (fd) {
                wavfile_close(fd);
            }
        }
        WavProgrammeHandler(const WavProgrammeHandler& other) = delete;
        WavProgrammeHandler& operator=(const WavProgrammeHandler& other) = delete;
        WavProgrammeHandler(WavProgrammeHandler&& other) = default;
        WavProgrammeHandler& operator=(WavProgrammeHandler&& other) = default;

        virtual void onFrameErrors(int frameErrors) override { (void)frameErrors; }
        virtual void onNewAudio(std::vector<int16_t>&& audioData, int sampleRate, bool isStereo, const string& mode) override
        {
            if (rate != sampleRate or stereo != isStereo) {
                cout << "[0x" << std::hex << SId << std::dec << "] " <<
                    "rate " << sampleRate << " stereo " << isStereo << " mode " << mode << endl;

                string filename = filePrefix + ".wav";
                if (fd) {
                    wavfile_close(fd);
                }
                fd = wavfile_open(filename.c_str(), sampleRate, isStereo ? 2 : 1);

                if (not fd) {
                    cerr << "Could not open wav file " << filename << endl;
                }
            }
            rate = sampleRate;
            stereo = isStereo;

            if (fd) {
                wavfile_write(fd, audioData.data(), audioData.size());
            }
        }

        virtual void onRsErrors(int rsErrors) override { (void)rsErrors; }
        virtual void onAacErrors(int aacErrors) override { (void)aacErrors; }
        virtual void onNewDynamicLabel(const std::string& label) override
        {
            cout << "[0x" << std::hex << SId << std::dec << "] " <<
                "DLS: " << label << endl;
        }

        virtual void onMOT(const std::vector<uint8_t>& data, int subtype) override { (void)data; (void)subtype; }

    private:
        uint32_t SId;
        string filePrefix;
        FILE* fd = nullptr;
        bool stereo = true;
        int rate = 0;
};


class RadioInterface : public RadioControllerInterface {
    public:
        virtual void onSNR(int snr) override { (void)snr; }
        virtual void onFrequencyCorrectorChange(int fine, int coarse) override { (void)fine; (void)coarse; }
        virtual void onSyncChange(char isSync) override { synced = isSync; }
        virtual void onSignalPresence(bool isSignal) override { (void)isSignal; }
        virtual void onServiceDetected(uint32_t sId, const std::string& label) override
        {
            cout << "New Service: 0x" << hex << sId << dec << " '" << label << "'" << endl;
        }

        virtual void onNewEnsembleName(const std::string& name) override
        {
            cout << "Ensemble name is: " << name << endl;
        }

        virtual void onDateTimeUpdate(const dab_date_time_t& dateTime) override
        {
            json j;
            j["UTCTime"] = {
                {"year", dateTime.year},
                {"month", dateTime.month},
                {"day", dateTime.day},
                {"hour", dateTime.hour},
                {"minutes", dateTime.minutes}
            };
            cout << j << endl;
        }

        virtual void onFIBDecodeSuccess(bool crcCheckOk, const uint8_t* fib) override { (void)crcCheckOk; (void)fib; }
        virtual void onNewImpulseResponse(std::vector<float>&& data) override { (void)data; }
        virtual void onNewNullSymbol(std::vector<DSPCOMPLEX>&& data) override { (void)data; }
        virtual void onConstellationPoints(std::vector<DSPCOMPLEX>&& data) override { (void)data; }
        virtual void onMessage(message_level_t level, const std::string& text) override
        {
            switch (level) {
                case message_level_t::Information:
                    cerr << "Info: " << text << endl;
                    break;
                case message_level_t::Error:
                    cerr << "Error: " << text << endl;
                    break;
            }
        }

        virtual void onTIIMeasurement(tii_measurement_t&& m) override
        {
            json j;
            j["TII"] = {
                {"comb", m.comb},
                {"pattern", m.pattern},
                {"delay", m.delay_samples},
                {"delay_km", m.getDelayKm()},
                {"error", m.error}
            };
            cout << j << endl;
        }

        bool synced = false;
};

struct options_t {
    string channel = "10B";
    string iqsource = "";
    string programme = "GRRIF";
    bool dump_programme = false;
    bool decode_all_programmes = false;
    bool decode_programmes_carousel = false;
    bool carousel_pad = false;
    int web_port = -1; // positive value means enable
    list<int> tests;
};

static void usage()
{
    cerr << "Usage: " << endl <<
#if defined(HAVE_ALSA)
        "Receive using RTLSDR, and play with ALSA:" << endl <<
        " welle-cli -c channel -p programme" << endl <<
        endl <<
        "Read an IQ file and play with ALSA:" << endl <<
        "IQ file format is complexf I/Q unless the filename ends with u8.iq" << endl <<
        " welle-cli -f file -p programme" << endl <<
        endl <<
#endif // defined(HAVE_ALSA)
        "Use -D to dump all programmes to files." << endl <<
        " welle-cli -c channel -D " << endl <<
        endl <<
        "Use -w to enable webserver, decode a programmes on demand." << endl <<
        " welle-cli -c channel -w port" << endl <<
        endl <<
        "Use -Dw to enable webserver, decode all programmes." << endl <<
        " welle-cli -c channel -Dw port" << endl <<
        endl <<
        "Use -Cw to enable webserver, decode programmes one by one in a carousel." << endl <<
        "This is useful if your machine cannot decode all programmes simultaneously, but" << endl <<
        "you still want to get an overview of the ensemble." << endl <<
        "Without the -P option, welle-cli will switch every 10 seconds." << endl <<
        "With the -P option, welle-cli will switch once DLS and a slide were decoded, staying at most" << endl <<
        "80 seconds on a given programme." << endl <<
        " welle-cli -c channel -Cw port" << endl <<
        " welle-cli -c channel -PCw port" << endl <<
        endl <<
        "Use -t test_number to run a test." << endl <<
        "To understand what the tests do, please see source code." << endl <<
        endl <<
        " examples: welle-cli -c 10B -p GRRIF" << endl <<
        "           welle-cli -f ./ofdm.iq -p GRRIF" << endl <<
        "           welle-cli -f ./ofdm.iq -t 1" << endl;
}

options_t parse_cmdline(int argc, char **argv)
{
    options_t options;
    int opt;
    while ((opt = getopt(argc, argv, "c:CdDf:hp:Pt:w:")) != -1) {
        switch (opt) {
            case 'c':
                options.channel = optarg;
                break;
            case 'C':
                options.decode_programmes_carousel = true;
                break;
            case 'd':
                options.dump_programme = true;
                break;
            case 'D':
                options.decode_all_programmes = true;
                break;
            case 'f':
                options.iqsource = optarg;
                break;
            case 'p':
                options.programme = optarg;
                break;
            case 'P':
                options.carousel_pad = true;
                break;
            case 'h':
                usage();
                exit(1);
            case 't':
                options.tests.push_back(std::atoi(optarg));
                break;
            case 'w':
                options.web_port = std::atoi(optarg);
                break;
            default:
                cerr << "Unknown option. Use -h for help" << endl;
                exit(1);
        }
    }

    if (options.decode_all_programmes and options.decode_programmes_carousel) {
        cerr << "Cannot select both -C and -D" << endl;
        exit(1);
    }

    return options;
}

int main(int argc, char **argv)
{
    cerr << "Hello this is welle-cli" << endl;
    auto options = parse_cmdline(argc, argv);

    RadioInterface ri;

    Channels channels;

    unique_ptr<CVirtualInput> in = nullptr;

    if (options.iqsource.empty()) {
        in.reset(CInputFactory::GetDevice(ri, "auto"));

        if (not in) {
            cerr << "Could not start device" << endl;
            return 1;
        }

#ifdef HAVE_SOAPYSDR
        if (in->getID() == CDeviceID::SOAPYSDR) {
            dynamic_cast<CSoapySdr*>(in.get())->setAntenna("TX/RX");
        }
#endif
    }
    else {
        // Run the tests without input throttling for max speed
        const bool throttle = options.tests.empty();
        const bool rewind = options.tests.empty();
        auto in_file = make_unique<CRAWFile>(ri, throttle, rewind);
        if (not in_file) {
            cerr << "Could not prepare CRAWFile" << endl;
            return 1;
        }

        if (options.iqsource.find("u8.iq") == string::npos) {
            in_file->setFileName(options.iqsource, "cf32");
        }
        else {
            in_file->setFileName(options.iqsource, "u8");
        }
        in = move(in_file);
    }

    in->setGain(6);
    in->setAgc(true);

    auto freq = channels.getFrequency(options.channel);
    in->setFrequency(freq);
    string service_to_tune = options.programme;

    if (not options.tests.empty()) {
        for (int test : options.tests) {
            run_test(test, in);
        }
    }
    else if (options.web_port != -1) {
        using DS = WebRadioInterface::DecodeStrategy;
        DS ds = DS::OnDemand;
        if (options.decode_all_programmes) {
            ds = DS::All;
        }
        else if (options.decode_programmes_carousel) {
            if (options.carousel_pad) {
                ds = DS::CarouselPAD;
            }
            else {
                ds = DS::Carousel10;
            }
        }
        WebRadioInterface wri(*in, options.web_port, ds);
        wri.serve();
    }
    else {
        RadioReceiver rx(ri, *in);

        rx.restart(false);

        cerr << "Wait for sync" << endl;
        while (not ri.synced) {
            this_thread::sleep_for(chrono::seconds(3));
        }

        if (options.decode_all_programmes) {
            using SId_t = uint32_t;
            map<SId_t, WavProgrammeHandler> phs;

            cerr << "Service list" << endl;
            for (const auto& s : rx.getServiceList()) {
                cerr << "  [0x" << std::hex << s.serviceId << std::dec << "] " <<
                    s.serviceLabel.utf8_label() << " ";
                for (const auto& sc : rx.getComponents(s)) {
                    cerr << " [component "  << sc.componentNr <<
                        " ASCTy: " <<
                        (sc.audioType() == AudioServiceComponentType::DAB ? "DAB" :
                         sc.audioType() == AudioServiceComponentType::DABPlus ? "DAB+" : "unknown") << " ]";

                    const auto& sub = rx.getSubchannel(sc);
                    cerr << " [subch " << sub.subChId << " bitrate:" << sub.bitrate() << " at SAd:" << sub.startAddr << "]";
                }
                cerr << endl;

                string dumpFilePrefix = s.serviceLabel.utf8_label();
                dumpFilePrefix.erase(std::find_if(dumpFilePrefix.rbegin(), dumpFilePrefix.rend(),
                            [](int ch) { return !std::isspace(ch); }).base(), dumpFilePrefix.end());

                WavProgrammeHandler ph(s.serviceId, dumpFilePrefix);
                phs.emplace(std::make_pair(s.serviceId, move(ph)));

                auto dumpFileName = dumpFilePrefix + ".msc";

                if (rx.addServiceToDecode(phs.at(s.serviceId), dumpFileName, s) == false) {
                    cerr << "Tune to " << service_to_tune << " failed" << endl;
                }
            }

            while (true) {
                cerr << "**** Enter '.' to quit." << endl;
                cin >> service_to_tune;
                if (service_to_tune == ".") {
                    break;
                }
            }
        }
        else {
#if defined(HAVE_ALSA)
            AlsaProgrammeHandler ph;
            while (not service_to_tune.empty()) {
                cerr << "Service list" << endl;
                for (const auto& s : rx.getServiceList()) {
                    cerr << "  [0x" << std::hex << s.serviceId << std::dec << "] " <<
                        s.serviceLabel.utf8_label() << " ";
                    for (const auto& sc : rx.getComponents(s)) {
                        cerr << " [component "  << sc.componentNr <<
                            " ASCTy: " <<
                            (sc.audioType() == AudioServiceComponentType::DAB ? "DAB" :
                             sc.audioType() == AudioServiceComponentType::DABPlus ? "DAB+" : "unknown") << " ]";

                        const auto& sub = rx.getSubchannel(sc);
                        cerr << " [subch " << sub.subChId << " bitrate:" << sub.bitrate() << " at SAd:" << sub.startAddr << "]";
                    }
                    cerr << endl;
                }

                bool service_selected = false;
                for (const auto s : rx.getServiceList()) {
                    if (s.serviceLabel.utf8_label().find(service_to_tune) != string::npos) {
                        service_selected = true;
                        string dumpFileName;
                        if (options.dump_programme) {
                            dumpFileName = s.serviceLabel.utf8_label();
                            dumpFileName.erase(std::find_if(dumpFileName.rbegin(), dumpFileName.rend(),
                                        [](int ch) { return !std::isspace(ch); }).base(), dumpFileName.end());
                            dumpFileName += ".msc";
                        }
                        if (rx.playSingleProgramme(ph, dumpFileName, s) == false) {
                            cerr << "Tune to " << service_to_tune << " failed" << endl;
                        }
                    }
                }
                if (not service_selected) {
                    cerr << "Could not tune to " << service_to_tune << endl;
                }

                cerr << "**** Please enter programme name. Enter '.' to quit." << endl;

                cin >> service_to_tune;
                if (service_to_tune == ".") {
                    break;
                }
                cerr << "**** Trying to tune to " << service_to_tune << endl;
            }
#else
            cerr << "Nothing to do, not ALSA support." << endl;
#endif // defined(HAVE_ALSA)
        }
    }

    return 0;
}
