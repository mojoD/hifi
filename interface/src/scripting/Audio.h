//
//  Audio.h
//  interface/src/scripting
//
//  Created by Zach Pomerantz on 28/5/2017.
//  Copyright 2017 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#ifndef hifi_scripting_Audio_h
#define hifi_scripting_Audio_h

#include "AudioScriptingInterface.h"
#include "AudioDevices.h"

namespace scripting {

class Audio : public AudioScriptingInterface {
    Q_OBJECT
    SINGLETON_DEPENDENCY

    Q_PROPERTY(bool muted READ isMuted WRITE setMuted NOTIFY changedMuted)
    Q_PROPERTY(bool noiseReduction READ noiseReductionEnabled WRITE enableNoiseReduction NOTIFY changedNoiseReduction)
    Q_PROPERTY(bool showMicMeter READ micMeterShown WRITE showMicMeter NOTIFY changedMicMeter)
    // TODO: Q_PROPERTY(bool reverb READ getReverb WRITE setReverb NOTIFY changedReverb)
    Q_PROPERTY(float inputVolume READ getInputVolume WRITE setInputVolume NOTIFY changedInputVolume)
    Q_PROPERTY(QString context READ getContext NOTIFY changedContext)
    Q_PROPERTY(AudioDevices* devices READ getDevices NOTIFY nop)

public:
    virtual ~Audio() {}

signals:
    void nop();
    void changedMuted(bool);
    void changedNoiseReduction(bool);
    void changedMicMeter(bool);
    void changedInputVolume(float);
    void changedContext(QString);

public slots:
    void onChangedMuted();
    void onChangedMicMeter(bool);
    void onChangedContext();
    void onInputChanged();

protected:
    Audio();

private:
    bool isMuted() const { return _isMuted; }
    bool noiseReductionEnabled() const { return _enableNoiseReduction; }
    bool micMeterShown()  const { return _showMicMeter; }
    float getInputVolume() const { return _inputVolume; }
    QString getContext() const;

    void setMuted(bool muted);
    void enableNoiseReduction(bool enable);
    void showMicMeter(bool show);
    void setInputVolume(float volume);

    float _inputVolume { 1.0f };
    bool _isMuted { false };
    bool _enableNoiseReduction { true };
    bool _showMicMeter { false };
    bool _contextIsHMD { false };

    AudioDevices* getDevices() { return &_devices; }
    AudioDevices _devices;
};

};

#endif // hifi_scripting_Audio_h
