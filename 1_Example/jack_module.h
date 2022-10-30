/**********************************************************************
 *          Copyright (c) 2022, Hogeschool voor de Kunsten Utrecht
 *                      Hilversum, the Netherlands
 *                          All rights reserved
 ***********************************************************************
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.
 *  If not, see <http://www.gnu.org/licenses/>.
 ***********************************************************************
 *
 *  File name     : jack_module.h
 *  System name   : jack_module
 *
 *  Description   : C++ abstraction for JACK Audio Connection Kit
 *
 *
 *  Authors       : Marc Groenewegen,
 *                  Wouter Ensink,
 *                  Daan Schrier
 *  E-mails       : marc.groenewegen@hku.nl,
 *                  wouter.ensink@student.hku.nl,
 *                  daan.schrier@student.hku.nl
 *
 **********************************************************************/


#pragma once

#include <jack/jack.h>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

struct AudioBuffer {
    const float** inputChannels;
    float** outputChannels;
    const int numInputChannels;
    const int numOutputChannels;
    const int numFrames;
};

class AudioCallback {
public:
    virtual void prepare (uint64_t sampleRate) {}
    virtual void process (AudioBuffer buffer) noexcept {}
};

class JackModule {
public:
    explicit JackModule (AudioCallback& audioCallback) : callback (audioCallback) {}

    ~JackModule() {
        end();
    }

    void init (int numInputs,
               int numOutputs,
               std::string_view inputClient = "system",
               std::string_view outputClient = "system") {
        init ("JackModule", numInputs, numOutputs, inputClient, outputClient);
    }

    void init (std::string_view clientName,
               int numInputs,
               int numOutputs,
               std::string_view inputClient = "system",
               std::string_view outputClient = "system") {
        setNumInputChannels (numInputs);
        setNumOutputChannels (numOutputs);
        client = jack_client_open (clientName.data(), JackNoStartServer, nullptr);

        if (client == nullptr) {
            throw std::runtime_error { "JACK server not running" };
        }

        jack_on_shutdown (client, jack_shutdown, nullptr);
        jack_set_process_callback (client, _wrap_jack_process_cb, this);

        inputPorts.clear();
        for (auto channel = 0; channel < numInputChannels; ++channel) {
            const auto name = "input_" + std::to_string (channel + 1);
            const auto port = jack_port_register (client, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
            inputPorts.push_back (port);
        }

        outputPorts.clear();
        for (auto channel = 0; channel < numOutputChannels; ++channel) {
            const auto name = "output_" + std::to_string (channel + 1);
            const auto port = jack_port_register (client, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
            outputPorts.push_back (port);
        }

        inputBuffers.resize (numInputChannels);
        outputBuffers.resize (numOutputChannels);

        if (jack_activate (client)) {
            throw std::runtime_error { "Cannot activate client" };
        }

        connectInput (inputClient);
        connectOutput (outputClient);

        callback.prepare (getSampleRate());
    }

    [[nodiscard]] uint64_t getSampleRate() const {
        return jack_get_sample_rate (client);
    }

private:
    using SampleType = jack_default_audio_sample_t;
    using UniquePortsPtr = std::unique_ptr<const char*, void (*) (const char**)>;

    AudioCallback& callback;
    jack_client_t* client = nullptr;

    std::vector<jack_port_t*> inputPorts;
    std::vector<jack_port_t*> outputPorts;
    std::vector<SampleType*> inputBuffers;
    std::vector<SampleType*> outputBuffers;

    int numInputChannels = 2;
    int numOutputChannels = 2;
    static constexpr auto MAX_INPUT_CHANNELS = 2;
    static constexpr auto MAX_OUTPUT_CHANNELS = 2;

    static int _wrap_jack_process_cb (jack_nframes_t numFrames, void* self) {
        return (reinterpret_cast<JackModule*> (self))->onProcess (numFrames);
    }

    int onProcess (jack_nframes_t numFrames) {
        for (auto channel = 0; channel < numInputChannels; ++channel) {
            inputBuffers[channel] = reinterpret_cast<SampleType*> (jack_port_get_buffer (inputPorts[channel], numFrames));
        }

        for (auto channel = 0; channel < numOutputChannels; ++channel) {
            outputBuffers[channel] = reinterpret_cast<SampleType*> (jack_port_get_buffer (outputPorts[channel], numFrames));
        }

        const auto buffer = AudioBuffer {
            .inputChannels = const_cast<const float**> (inputBuffers.data()),
            .outputChannels = outputBuffers.data(),
            .numInputChannels = static_cast<int> (inputBuffers.size()),
            .numOutputChannels = static_cast<int> (outputBuffers.size()),
            .numFrames = static_cast<int> (numFrames),
        };
        callback.process (buffer);

        return 0;
    }

    static int countPorts (const char** ports) {
        auto numPorts = 0;
        while (ports[numPorts])
            ++numPorts;
        return numPorts;
    }

    static auto makePortsPtr (const char** ports) -> UniquePortsPtr {
        return {
            ports,
            [] (auto** p) { free (p); }
        };
    }

    auto findPorts (std::string_view clientName, uint64_t flags) -> UniquePortsPtr {
        if (auto ports = makePortsPtr (jack_get_ports (client, clientName.data(), nullptr, flags))) {
            return ports;
        }

        throw std::runtime_error {
            "Cannot find capture ports associated with " + std::string { clientName } + "."
        };
    }

    void setNumInputChannels (int n) {
        if (n > MAX_INPUT_CHANNELS || n < 0) {
            throw std::runtime_error { "Invalid number of input channels" };
        }
        numInputChannels = n;
    }

    void setNumOutputChannels (int n) {
        if (n > MAX_OUTPUT_CHANNELS || n < 0) {
            throw std::runtime_error { "Invalid number of output channels" };
        }
        numOutputChannels = n;
    }

    void connectInput (std::string_view inputClient) {
        if (numInputChannels > 0) {
            auto ports = findPorts (inputClient.data(), JackPortIsOutput);

            // When you hit this assert, it means you want more inputs than are available in Jack.
            // Please consider lowering the number of input channels you want.
            assert (countPorts (ports.get()) >= numInputChannels);

            for (auto channel = 0; channel < numInputChannels; ++channel) {
                if (jack_connect (client, ports.get()[channel], jack_port_name (inputPorts[channel]))) {
                    throw std::runtime_error { "Cannot connect input ports" };
                }
            }
        }
    }

    void connectOutput (std::string_view outputClient) {
        if (numOutputChannels > 0) {
            auto ports = findPorts (outputClient.data(), JackPortIsInput);

            // When you hit this assert, it means you want more outputs than are available in Jack.
            // Please consider lowering the number of output channels you want.
            assert (countPorts (ports.get()) >= numOutputChannels);

            for (auto channel = 0; channel < numOutputChannels; ++channel) {
                if (jack_connect (client, jack_port_name (outputPorts[channel]), ports.get()[channel])) {
                    throw std::runtime_error { "Cannot connect output ports" };
                }
            }
        }
    }

    void end() {
        jack_deactivate (client);

        for (auto port : inputPorts)
            jack_port_disconnect (client, port);

        for (auto port : outputPorts)
            jack_port_disconnect (client, port);
    }

    static void jack_shutdown ([[maybe_unused]] void* arg) {
        exit (1);
    }
};