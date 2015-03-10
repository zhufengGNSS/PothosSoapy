// Copyright (c) 2015-2015 Josh Blum
// SPDX-License-Identifier: BSL-1.0

#include <Pothos/Framework.hpp>

/***********************************************************************
 * |PothosDoc Channel Aligner
 *
 * The channel aligner block consumes N input streams and uses timestamps
 * and selective dropping to output N time-aligned output streams.
 *
 * <ul>
 * <li>This is a zero-copy block, buffers are passively forwarded to the output.</li>
 * <li>This is a zero-conf block, sample rate is passed via inline stream labels.</li>
 * <li>This type agnostic block, data type is determined from the input buffers.</li>
 * </ul>
 *
 * |category /SDR
 * |keywords time align channel mimo
 *
 * |param numChans[Num Channels] The number of channels.
 * |default 2
 * |widget SpinBox(minimum=2)
 * |preview disable
 *
 * |factory /sdr/channel_aligner()
 * |initializer setNumChannels(numChans)
 **********************************************************************/
class ChannelAligner : public Pothos::Block
{
public:
    static Block *make(void)
    {
        return new ChannelAligner();
    }

    ChannelAligner(void):
        _sampleRate(1.0)
    {
        this->setupInput(0);
        this->setupOutput(0, "", this->uid()); //unique domain because of buffer forwarding
        this->registerCall(this, POTHOS_FCN_TUPLE(ChannelAligner, setNumChannels));
    }

    void setNumChannels(const size_t numChans)
    {
        if (numChans < 2) throw Pothos::RangeException(
            "ChannelAligner::setNumChannels("+std::to_string(numChans)+")", "require numChans >= 2");

        for (size_t i = this->inputs().size(); i < numChans; i++)
        {
            this->setupInput(i, this->input(0)->dtype());
        }

        for (size_t i = this->outputs().size(); i < numChans; i++)
        {
            this->setupOutput(i, this->output(0)->dtype());
        }
    }

    void activate(void)
    {
        _nextTicks.resize(this->inputs().size());
    }

    void work(void);

private:
    double _sampleRate;
    std::vector<long long> _nextTicks;
};

void ChannelAligner::work(void)
{
    //search all inputs for relevant labels
    for (auto input : this->inputs())
    {
        for (const auto &label : input->labels())
        {
            if (label.index >= input->elements()) continue;

            //extract rx time and sample rate
            if (label.id == "rxRate")
            {
                _sampleRate = label.data.convert<double>();
            }
            else if (label.id == "rxTime")
            {
                const auto timeNs = label.data.convert<long long>();
                auto ticks = (timeNs*_sampleRate)/1e9;
                ticks -= label.index/input->buffer().dtype.size();
                _nextTicks[input->index()] = ticks;
            }
        }
    }

    //consume and dont forward inputs to force alignment
    size_t alignIndex = 0;
    auto alignTicks = _nextTicks[alignIndex++];
    while (alignIndex < this->inputs().size())
    {
        const auto frontTicks = _nextTicks[alignIndex];

        //front ticks are equal, check next channel
        if (frontTicks == alignTicks) alignIndex++;

        //front ticks are newer, reset ticks, start loop again
        else if (frontTicks > alignTicks)
        {
            alignTicks = frontTicks;
            alignIndex = 0;
        }

        //front ticks are older, consume and return
        else if (frontTicks < alignTicks)
        {
            auto input = this->input(alignIndex);
            const size_t items = (alignTicks - frontTicks);
            const size_t bytes = items*input->buffer().dtype.size();
            input->consume(std::min(bytes, input->elements()));
            return; //we get called again ASAP if inputs are available
        }
    }

    //we are in alignment, forward all outputs
    for (auto input : this->inputs())
    {
        this->output(input->index())->postBuffer(input->buffer());
        const auto ticks = input->elements()/input->buffer().dtype.size();
        _nextTicks[input->index()] += ticks;
        input->consume(input->elements());
    }
}

static Pothos::BlockRegistry registerChannelAligner(
    "/sdr/channel_aligner", &ChannelAligner::make);