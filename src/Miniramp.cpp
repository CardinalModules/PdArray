#include "plugin.hpp"
#include "Widgets.hpp"
#include "Util.hpp"

#include <algorithm> // std::replace

const float MIN_EXPONENT = -3.0f;
const float MAX_EXPONENT = 1.0f;

// quick and dirty copy-paste of LittleUtils PulseGenerator.
// TODO: move modules that are shared with LittleUtils common folder
// TODO: show sample count corresponding to duration on mouse over (see ParamWidget::onEnter)
// TODO: add trigger buttton (another reason to remove cv)

// based on PulseGeneraotr in include/util/digital.hpp
struct CustomPulseGenerator {
	float time;
	float triggerDuration;
	bool finished; // the output is the inverse of this

	CustomPulseGenerator() {
		reset();
	}
	/** Immediately resets the state to LOW */
	void reset() {
		time = 0.f;
		triggerDuration = 0.f;
		finished = true;
	}
	/** Advances the state by `deltaTime`. Returns whether the pulse is in the HIGH state. */
	bool process(float deltaTime) {
		time += deltaTime;
		if(!finished) finished = time >= triggerDuration;
		return !finished;
	}
	/** Begins a trigger with the given `triggerDuration`. */
	void trigger(float triggerDuration) {
		// retrigger even with a shorter duration
		time = 0.f;
		finished = false;
		this->triggerDuration = triggerDuration;
	}
};

struct Miniramp : Module {
	enum ParamIds {
		RAMP_LENGTH_PARAM,
		CV_AMT_PARAM,
		LIN_LOG_MODE_PARAM,
		NUM_PARAMS
	};
	enum InputIds {
		TRIG_INPUT,
		RAMP_LENGTH_INPUT,
		RESET_INPUT,
		NUM_INPUTS
	};
	enum OutputIds {
		RAMP_OUTPUT,
		GATE_OUTPUT,
		EOC_OUTPUT,
		FINISH_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightIds {
		RAMP_LIGHT,
		GATE_LIGHT,
		EOC_LIGHT,
		FINISH_LIGHT,
		NUM_LIGHTS
	};
	enum RampFinishedMode {
		RAMP_FINISHED_0,
		RAMP_FINISHED_10,
		NUM_RAMP_FINISHED_MODES
	};

	dsp::SchmittTrigger inputTrigger[MAX_POLY_CHANNELS];
	dsp::SchmittTrigger resetTrigger[MAX_POLY_CHANNELS];
	CustomPulseGenerator gateGen[MAX_POLY_CHANNELS];
	CustomPulseGenerator eocGen[MAX_POLY_CHANNELS];
	float ramp_base_duration = 0.5f; // ramp duration without CV
	float ramp_duration;
	float cv_scale = 0.f; // cv_scale = +- 1 -> 10V CV changes duration by +-10s
	RampFinishedMode rampFinishedMode = RAMP_FINISHED_0;

	Miniramp() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);

		configParam(Miniramp::RAMP_LENGTH_PARAM, 0.f, 10.f,
					// 0.5s in log scale
					//rescale(-0.30103f, MIN_EXPONENT, MAX_EXPONENT, 0.f,10.f)
					5.f, // 0.1s in log mode, 5s in lin mode
					"Ramp duration");

		configParam(Miniramp::CV_AMT_PARAM, -1.f, 1.f, 0.f, "Ramp duration CV mod amount");
		configSwitch(Miniramp::LIN_LOG_MODE_PARAM, 0.f, 1.f, 1.f, "Ramp duration adjust mode", { "Linear", "Logarithmic" });
		configInput(TRIG_INPUT, "Trigger");
		configInput(RAMP_LENGTH_INPUT, "Ramp duration CV modulation");
		configInput(RESET_INPUT, "Reset/stop ramp");
		configOutput(RAMP_OUTPUT, "Ramp");
		configOutput(GATE_OUTPUT, "Gate");
		configOutput(EOC_OUTPUT, "End of cycle");
		configOutput(FINISH_OUTPUT, "Ramp finished");

		ramp_duration = ramp_base_duration;
		for(int c = 0; c < MAX_POLY_CHANNELS; c++) {
			gateGen[c].reset();
			eocGen[c].reset();
		}
	}

	json_t *dataToJson() override {
		json_t *root = json_object();
		json_object_set_new(root, "rampFinishedMode", json_integer(rampFinishedMode));

		return root;
	}

	void dataFromJson(json_t *root) override {
		json_t *rampFinished_J = json_object_get(root, "rampFinishedMode");
		if(rampFinished_J) {
			int rfm = int(json_integer_value(rampFinished_J));
			if(rfm < NUM_RAMP_FINISHED_MODES) {
				rampFinishedMode = static_cast<RampFinishedMode>(rfm);
			}
		}
	}

	void process(const ProcessArgs &args) override;

};

void Miniramp::process(const ProcessArgs &args) {
	float deltaTime = args.sampleTime;
	const int channels = inputs[TRIG_INPUT].getChannels();

	// handle duration knob and CV
	float knob_value = params[RAMP_LENGTH_PARAM].getValue();
	float cv_amt = params[CV_AMT_PARAM].getValue();
	float cv_voltage = inputs[RAMP_LENGTH_INPUT].getVoltage();

	if(params[LIN_LOG_MODE_PARAM].getValue() < 0.5f) {
		// linear mode
		cv_scale = cv_amt;
		ramp_base_duration = knob_value;
	} else {
		// logarithmic mode
		float exponent = rescale(knob_value,
				0.f, 10.f, MIN_EXPONENT, MAX_EXPONENT);

		float cv_exponent = rescale(fabs(cv_amt), 0.f, 1.f,
				MIN_EXPONENT, MAX_EXPONENT);

		// decrease exponent by one so that 10V maps to 1.0 (100%) CV.
		cv_scale = powf(10.0f, cv_exponent - 1.f) * signum(cv_amt); // take sign into account

		ramp_base_duration = powf(10.0f, exponent);
	}
	ramp_duration = clamp(ramp_base_duration + cv_voltage * cv_scale, 0.f, 10.f);

	for(int c = 0; c < std::max(channels, 1); c++) {
		bool triggered = inputTrigger[c].process(rescale(
					inputs[TRIG_INPUT].getVoltage(c), 0.1f, 2.f, 0.f, 1.f));

		//TODO: polyphony: if there's only one channel, reset all ramps
		if (resetTrigger[c].process(rescale(
			inputs[RESET_INPUT].getVoltage(c),
			0.1f, 2.0f,
			0.0f, 1.0f
		))) {
			// reset everything
			gateGen[c].reset();
			eocGen[c].reset();
		} else if(triggered && ramp_duration > 0.f) {
			gateGen[c].trigger(ramp_duration);
		}

		// update trigger duration even in the middle of a trigger
		gateGen[c].triggerDuration = ramp_duration;

		bool gate_prev = !gateGen[c].finished;
		bool gate = gateGen[c].process(deltaTime);

		// gate was finished, start EOC
		if(gate_prev && !gate) {
			eocGen[c].trigger(1e-3f);
		}

		bool eoc_pulse = eocGen[c].process(deltaTime);

		float ramp_v;
		if(gate) {
			ramp_v = clamp(gateGen[c].time/gateGen[c].triggerDuration * 10.f, 0.f, 10.f);
		} else {
			if(rampFinishedMode == RAMP_FINISHED_0) {
				ramp_v = 0;
			} else {
				ramp_v = 10;
			}
		}

		outputs[RAMP_OUTPUT].setVoltage(ramp_v, c);
		outputs[GATE_OUTPUT].setVoltage(gate ? 10.0f : 0.0f, c);
		outputs[EOC_OUTPUT].setVoltage(eoc_pulse ? 10.0f : 0.0f, c);
		outputs[FINISH_OUTPUT].setVoltage(gate ? 0.0f : 10.0f, c);

		//TODO: figure out lights for polyphonic mode
		lights[RAMP_LIGHT].setSmoothBrightness(outputs[RAMP_OUTPUT].value * 1e-1f, deltaTime);
		lights[GATE_LIGHT].setSmoothBrightness(outputs[GATE_OUTPUT].value, deltaTime);
		lights[EOC_LIGHT].setSmoothBrightness(outputs[EOC_OUTPUT].value, deltaTime);
		lights[FINISH_LIGHT].setSmoothBrightness(outputs[FINISH_OUTPUT].value, deltaTime);

	}
	outputs[RAMP_OUTPUT].setChannels(channels);
	outputs[GATE_OUTPUT].setChannels(channels);
}

struct MsDisplayWidget : TextBox {
	Miniramp *module;
	bool cvLabelStatus = false; // whether to show 'cv'
	float previous_displayed_value = 0.f;
	float cvDisplayTime = 2.f;

	GUITimer cvDisplayTimer;

	MsDisplayWidget(Miniramp *m) : TextBox() {
		module = m;
		box.size = Vec(65, 20);
		letterSpacing = -2.0f;
		textAlign = NVG_ALIGN_LEFT | NVG_ALIGN_TOP;
	}

	void updateDisplayValue(float v) {
		// only update/do stringf if value is changed
		if(v != previous_displayed_value) {
			std::string s;
			previous_displayed_value = v;
			s = string::f("%#.4f", v);
			// hacky way to make monospace fonts prettier
			std::replace(s.begin(), s.end(), '0', 'O');
			// if the length is 10.0, we will have too many decimal digits, truncate
			s = s.substr(0, 6);
			setText(s);
		}
	}

	void draw(const DrawArgs &args) override {
		TextBox::draw(args);
		const auto vg = args.vg;
		nvgScissor(vg, 0, 0, box.size.x, box.size.y);

		std::shared_ptr<Font> font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/RobotoMono-Bold.ttf"));
		if(font) {
			nvgFillColor(vg, textColor);
			nvgFontFaceId(vg, font->handle);

			nvgFontSize(vg, 12);
			nvgTextLetterSpacing(vg, 0.f);
			nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
			nvgText(vg, textOffset.x + 2, textOffset.y + 14, " s", NULL);

			if(cvLabelStatus) {
				nvgText(vg, 3, textOffset.y + 14, "cv", NULL);
			}
		}

		nvgResetScissor(vg);
	}

	void triggerCVDisplay() {
		cvDisplayTimer.trigger(cvDisplayTime);
	}

	void step() override {
		TextBox::step();
		cvLabelStatus = cvDisplayTimer.process();
		if(module) {
			updateDisplayValue(cvLabelStatus ? fabs(module->cv_scale) * 10.f : module->ramp_duration);
		}
	}

};

struct CustomTrimpot : Trimpot {
	MsDisplayWidget *display;
	CustomTrimpot(): Trimpot() {};

	void onDragMove(const event::DragMove &e) override {
		Trimpot::onDragMove(e);
		display->triggerCVDisplay();
	}
};

template <typename T>
struct MinirampEnumChildMenuItem : MenuItem {
	Miniramp *module;
	// Miniramp::RampFinishedMode or Miniramp::GateEOCMode
	T mode;
	// Miniramp->rampFinishedMode or Miniramp->gateEOCMode
	T *modeParam;

	MinirampEnumChildMenuItem(
		Miniramp *m,
		T pMode,
		T *pModeParam,
		std::string label
	) : MenuItem() {
		module = m;
		mode = pMode;
		modeParam = pModeParam;
		text = label;
		rightText = CHECKMARK(*modeParam == mode);
	}

	void onAction(const event::Action &e) override {
		*modeParam = mode;
	}
};


struct MinirampFinishedModeChildMenuItem : MinirampEnumChildMenuItem<Miniramp::RampFinishedMode> {
	MinirampFinishedModeChildMenuItem(
		Miniramp *m,
		Miniramp::RampFinishedMode pMode,
		std::string label
	) : MinirampEnumChildMenuItem(m, pMode, &m->rampFinishedMode, label) {};
};

struct MinirampFinishedModeMenuItem : MenuItemWithRightArrow {
	Miniramp *module;
	Menu *createChildMenu() override {
		Menu *menu = new Menu();
		menu->addChild(new MinirampFinishedModeChildMenuItem(
			module,
			Miniramp::RAMP_FINISHED_0,
			"0V"
		));
		menu->addChild(new MinirampFinishedModeChildMenuItem(
			module,
			Miniramp::RAMP_FINISHED_10,
			"10V"
		));
		return menu;
	}
};

struct MinirampWidget : ModuleWidget {
	Miniramp *module;
	MsDisplayWidget *msDisplay;

	MinirampWidget(Miniramp *module) {
		setModule(module);
		this->module = module;
		setPanel(APP->window->loadSvg(asset::plugin(pluginInstance, "res/Miniramp.svg")));

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH * 3, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH * 3, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		addParam(createParamCentered<RoundBlackKnob>(Vec(22.5, 37.5), module,
					Miniramp::RAMP_LENGTH_PARAM));

		addParam(createParam<CKSS>(Vec(20, 100), module, Miniramp::LIN_LOG_MODE_PARAM));

		addInput(createInputCentered<PJ301MPort>(Vec(20, 147), module, Miniramp::RAMP_LENGTH_INPUT));
		addInput(createInputCentered<PJ301MPort>(Vec(20, 192), module, Miniramp::TRIG_INPUT));
		addInput(createInputCentered<PJ301MPort>(Vec(55, 192), module, Miniramp::RESET_INPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(20, 240), module, Miniramp::RAMP_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(55, 240), module, Miniramp::GATE_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(20, 288), module, Miniramp::EOC_OUTPUT));
		addOutput(createOutputCentered<PJ301MPort>(Vec(55, 288), module, Miniramp::FINISH_OUTPUT));

		addChild(createTinyLightForPort<GreenLight>(Vec(20, 240), module, Miniramp::RAMP_LIGHT));
		addChild(createTinyLightForPort<GreenLight>(Vec(55, 240), module, Miniramp::GATE_LIGHT));
		addChild(createTinyLightForPort<GreenLight>(Vec(20, 288), module, Miniramp::EOC_LIGHT));
		addChild(createTinyLightForPort<GreenLight>(Vec(55, 288), module, Miniramp::FINISH_LIGHT));

		msDisplay = new MsDisplayWidget(module);
		msDisplay->box.pos = Vec(5, 318);
		addChild(msDisplay);

		auto cvKnob = createParamCentered<CustomTrimpot>(Vec(55, 147), module,
				Miniramp::CV_AMT_PARAM);
		cvKnob->display = msDisplay;
		addParam(cvKnob);

	}

	void appendContextMenu(ui::Menu *menu) override {
		if(module) {
			auto *finishModeMenuItem = new MinirampFinishedModeMenuItem();
			finishModeMenuItem->text = "Ramp value when finished";
			finishModeMenuItem->module = module;
			menu->addChild(finishModeMenuItem);
		}
	}

};


Model *modelMiniramp = createModel<Miniramp, MinirampWidget>("Miniramp");
