#include "obs-shader-filter.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs_shader_filter", "en-US")
#define blog(level, msg, ...) blog(level, "shader-filter: " msg, ##__VA_ARGS__)

#ifdef _WIN32
#include <Windows.h>
#elif __linux__ || __FreeBSD__
#include <X11/extensions/Xfixes.h>
#define xdisp (XCompcap::disp())
#endif

static const float farZ = 2097152.0f; // 2 pow 21
static const float nearZ = 1.0f / farZ;

static void sidechain_capture(void *p, obs_source_t *source, const struct audio_data *audio_data, bool muted);

static bool shader_filter_reload_effect_clicked(obs_properties_t *props, obs_property_t *property, void *data);

static bool shader_filter_file_name_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings);

static void getMouseCursor(void *data);
static void getScreenSizes(void *data);

static const char *shader_filter_texture_file_filter = "Textures (*.bmp *.tga *.png *.jpeg *.jpg *.gif);;";

static const char *shader_filter_media_file_filter =
"Video Files (*.mp4 *.ts *.mov *.wmv *.flv *.mkv *.avi *.gif *.webm);;";

#define M_PI_D 3.141592653589793238462643383279502884197169399375
static double hlsl_clamp(double in, double min, double max)
{
	if (in < min)
		return min;
	if (in > max)
		return max;
	return in;
}

static double dmin(double a, double b)
{
	return a < b ? a : b;
}

static double dmax(double a, double b)
{
	return a > b ? a : b;
}

static double hlsl_degrees(double radians)
{
	return radians * (180.0 / M_PI_D);
}

static double hlsl_rad(double degrees)
{
	return degrees * (M_PI_D / 180.0);
}

static double audio_mel_from_hz(double hz)
{
	return 2595 * log10(1 + hz / 700.0);
}

static double audio_hz_from_mel(double mel)
{
	return 700 * (pow(10, mel / 2595) - 1);
}

static double pi(void)
{
	return 3.141592653589793238462643383279502884197169399375;
}
/*float precision 2.71828182845904523536*/
static double e(void)
{
	return 2.718281828459045235360287471352662497757247093699;
}

static double dceil(double d)
{
	return ceil(d);
};

static double dfloor(double d)
{
	return floor(d);
}

static double fac(double a)
{/* simplest version of fac */
	if (a < 0.0)
		return NAN;
	if (a > UINT_MAX)
		return INFINITY;
	unsigned int ua = (unsigned int)(a);
	unsigned long int result = 1, i;
	for (i = 1; i <= ua; i++) {
		if (i > ULONG_MAX / result)
			return INFINITY;
		result *= i;
	}
	return (double)result;
}

static double ncr(double n, double r)
{
	if (n < 0.0 || r < 0.0 || n < r) return NAN;
	if (n > UINT_MAX || r > UINT_MAX) return INFINITY;
	unsigned long int un = (unsigned int)(n), ur = (unsigned int)(r), i;
	unsigned long int result = 1;
	if (ur > un / 2) ur = un - ur;
	for (i = 1; i <= ur; i++) {
		if (result > ULONG_MAX / (un - ur + i))
			return INFINITY;
		result *= un - ur + i;
		result /= i;
	}
	return result;
}

static double npr(double n, double r)
{
	return ncr(n, r) * fac(r);
}

const static double flt_max = FLT_MAX;
const static double flt_min = FLT_MIN;
const static double int_min = INT_MIN;
const static double int_max = INT_MAX;
static double       sample_rate;
static double       output_channels;
static std::string  dir[4] = { "left", "right", "top", "bottom" };

/* Additional likely to be used functions for mathmatical expressions */
static void prepFunctions(std::vector<te_variable> *vars, ShaderSource *filter)
{
	UNUSED_PARAMETER(filter);
	std::vector<te_variable> funcs = {
	{"clamp", hlsl_clamp, TE_FUNCTION3 | TE_FLAG_PURE},
	{"channels", &output_channels}, {"degrees", hlsl_degrees, TE_FUNCTION1 | TE_FLAG_PURE},
	{"float_max", &flt_max},
	{"float_min", &flt_min}, {"hz_from_mel", audio_hz_from_mel, TE_FUNCTION1 | TE_FLAG_PURE},
	{"int_max", &int_max}, {"int_min", &int_min},
	{"key", &filter->_key}, {"key_pressed", &filter->_keyUp},
	{"sample_rate", &sample_rate},
	{"mel_from_hz", audio_mel_from_hz, TE_FUNCTION1 | TE_FLAG_PURE},
	{"mouse_click_x", &filter->_mouseClickX},
	{"mouse_click_y", &filter->_mouseClickY},
	{"mouse_pos_x", &filter->_mouseX}, {"mouse_pos_y", &filter->_mouseY},
	{"mouse_type", &filter->_mouseType},
	{"mouse_up", &filter->_mouseUp},
	{"mouse_wheel_delta_x", &filter->_mouseWheelDeltaX},
	{"mouse_wheel_delta_y", &filter->_mouseWheelDeltaY}, {"mouse_wheel_x", &filter->_mouseWheelX},
	{"mouse_wheel_y", &filter->_mouseWheelY}, {"mouse_leave", &filter->_mouseLeave},
	{"primary_screen_height", &filter->_primaryScreenHeight}, {"primary_screen_width", &filter->_primaryScreenWidth},
	{"radians", hlsl_rad, TE_FUNCTION1 | TE_FLAG_PURE}, {"random", random_double, TE_FUNCTION2},
	{"screen_height", &filter->_wholeScreenHeight},
	{"screen_width", &filter->_wholeScreenWidth},
	{"screen_mouse_pos_x", &filter->_screenMousePosX}, {"screen_mouse_pos_y", &filter->_screenMousePosY},
	{"screen_mouse_visible", &filter->_screenMouseVisible},
	{"max", dmax, TE_FUNCTION2 | TE_FLAG_PURE, 0},
	{"min", dmin, TE_FUNCTION2 | TE_FLAG_PURE, 0},
	/* Basic functions originally included in TinyExpr */
	{"abs", static_cast<double(*)(double)>(fabs),     TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"acos", static_cast<double(*)(double)>(acos),    TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"asin", static_cast<double(*)(double)>(asin),    TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"atan", static_cast<double(*)(double)>(atan),    TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"atan2", static_cast<double(*)(double, double)>(atan2),  TE_FUNCTION2 | TE_FLAG_PURE, 0},
	{"ceil", static_cast<double(*)(double)>(dceil),   TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"cos", static_cast<double(*)(double)>(cos),      TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"cosh", static_cast<double(*)(double)>(cosh),    TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"e", static_cast<double(*)()>(e), TE_FUNCTION0 | TE_FLAG_PURE, 0},
	{"exp", static_cast<double(*)(double)>(exp),      TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"fac", static_cast<double(*)(double)>(fac),      TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"floor", static_cast<double(*)(double)>(dfloor), TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"ln", static_cast<double(*)(double)>(log),       TE_FUNCTION1 | TE_FLAG_PURE, 0},
    #ifdef TE_NAT_LOG
	{"log", static_cast<double(*)(double)>(log),      TE_FUNCTION1 | TE_FLAG_PURE, 0},
    #else
	{"log", static_cast<double(*)(double)>(log10),    TE_FUNCTION1 | TE_FLAG_PURE, 0},
    #endif
	{"log10", static_cast<double(*)(double)>(log10),  TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"ncr", static_cast<double(*)(double, double)>(ncr),      TE_FUNCTION2 | TE_FLAG_PURE, 0},
	{"npr", static_cast<double(*)(double, double)>(npr),      TE_FUNCTION2 | TE_FLAG_PURE, 0},
	{"pi", static_cast<double(*)()>(pi),              TE_FUNCTION0 | TE_FLAG_PURE, 0},
	{"pow", static_cast<double(*)(double, double)>(pow),      TE_FUNCTION2 | TE_FLAG_PURE, 0},
	{"sin", static_cast<double(*)(double)>(sin),      TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"sinh", static_cast<double(*)(double)>(sinh),    TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"sqrt", static_cast<double(*)(double)>(sqrt),    TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"tan", static_cast<double(*)(double)>(tan),      TE_FUNCTION1 | TE_FLAG_PURE, 0},
	{"tanh", static_cast<double(*)(double)>(tanh),    TE_FUNCTION1 | TE_FLAG_PURE, 0} };
	vars->reserve(vars->size() + funcs.size());
	vars->insert(vars->end(), funcs.begin(), funcs.end());

}

std::string toSnakeCase(std::string str)
{
	size_t i;
	char   c;
	for (i = 0; i < str.size(); i++) {
		c = str[i];
		if (isupper(c)) {
			str.insert(i++, "_");
			str[i] = (char)tolower(c);
		}
	}
	return str;
}

std::string toCamelCase(std::string str)
{
	size_t i;
	char   c, c2;
	for (i = 0; i < str.size() - 1; i++) {
		c = str[i];
		if (c == '_') {
			c2 = str[i + 1];
			if (islower(c2)) {
				str.erase(i);
				if (i < str.size())
					str[i] = (char)toupper(c);
			}
		}
	}
	return str;
}

int getDataSize(enum gs_shader_param_type type)
{
	switch (type) {
	case GS_SHADER_PARAM_VEC4:
	case GS_SHADER_PARAM_INT4:
		return 4;
	case GS_SHADER_PARAM_VEC3:
	case GS_SHADER_PARAM_INT3:
		return 3;
	case GS_SHADER_PARAM_VEC2:
	case GS_SHADER_PARAM_INT2:
		return 2;
	case GS_SHADER_PARAM_FLOAT:
	case GS_SHADER_PARAM_INT:
	case GS_SHADER_PARAM_BOOL:
		return 1;
	case GS_SHADER_PARAM_MATRIX4X4:
		return 16;
	}
	return 0;
}

bool isFloatType(enum gs_shader_param_type type)
{
	switch (type) {
	case GS_SHADER_PARAM_VEC4:
	case GS_SHADER_PARAM_VEC3:
	case GS_SHADER_PARAM_VEC2:
	case GS_SHADER_PARAM_FLOAT:
	case GS_SHADER_PARAM_MATRIX4X4:
		return true;
	}
	return false;
}

bool isIntType(enum gs_shader_param_type type)
{
	switch (type) {
	case GS_SHADER_PARAM_INT:
	case GS_SHADER_PARAM_INT2:
	case GS_SHADER_PARAM_INT3:
	case GS_SHADER_PARAM_INT4:
		return true;
	}
	return false;
}

class EVal {
public:
	float defaultFloat = 0.0;
	int   defaultInt = 0;

	void *               data = nullptr;
	size_t               size = 0;
	gs_shader_param_type type = GS_SHADER_PARAM_UNKNOWN;
	EVal()
	{
	};
	~EVal()
	{
		if (data)
			bfree(data);
	};

	operator std::vector<float>()
	{
		std::vector<float> dFloat;
		std::vector<int>   dInt;
		std::vector<bool>  dBool;
		float *            ptrFloat = static_cast<float *>(data);
		int *              ptrInt = static_cast<int *>(data);
		bool *             ptrBool = static_cast<bool *>(data);

		size_t i;
		size_t len;

		switch (type) {
		case GS_SHADER_PARAM_BOOL:
			len = size / sizeof(bool);
			dFloat.reserve(len);
			dBool.assign(ptrBool, ptrBool + len);
			for (i = 0; i < dBool.size(); i++)
				dFloat.push_back(dBool[i]);
			break;
		case GS_SHADER_PARAM_FLOAT:
		case GS_SHADER_PARAM_VEC2:
		case GS_SHADER_PARAM_VEC3:
		case GS_SHADER_PARAM_VEC4:
		case GS_SHADER_PARAM_MATRIX4X4:
			len = size / sizeof(float);
			dFloat.assign(ptrFloat, ptrFloat + len);
			break;
		case GS_SHADER_PARAM_INT:
		case GS_SHADER_PARAM_INT2:
		case GS_SHADER_PARAM_INT3:
		case GS_SHADER_PARAM_INT4:
			len = size / sizeof(int);
			dFloat.reserve(len);
			dInt.assign(ptrInt, ptrInt + len);
			for (i = 0; i < dInt.size(); i++)
				dFloat.push_back((float)dInt[i]);
			break;
		}
		return dFloat;
	}

	operator std::vector<int>()
	{
		std::vector<float> dFloat;
		std::vector<int>   dInt;
		std::vector<bool>  dBool;
		float *            ptrFloat = static_cast<float *>(data);
		int *              ptrInt = static_cast<int *>(data);
		bool *             ptrBool = static_cast<bool *>(data);

		size_t i;
		size_t len;

		switch (type) {
		case GS_SHADER_PARAM_BOOL:
			len = size / sizeof(bool);
			dInt.reserve(len);
			dBool.assign(ptrBool, ptrBool + len);
			for (i = 0; i < dBool.size(); i++)
				dInt.push_back(dBool[i]);
			break;
		case GS_SHADER_PARAM_FLOAT:
		case GS_SHADER_PARAM_VEC2:
		case GS_SHADER_PARAM_VEC3:
		case GS_SHADER_PARAM_VEC4:
		case GS_SHADER_PARAM_MATRIX4X4:
			len = size / sizeof(float);
			dInt.reserve(len);
			dFloat.assign(ptrFloat, ptrFloat + len);
			for (i = 0; i < dFloat.size(); i++)
				dInt.push_back((int)dFloat[i]);
			break;
		case GS_SHADER_PARAM_INT:
		case GS_SHADER_PARAM_INT2:
		case GS_SHADER_PARAM_INT3:
		case GS_SHADER_PARAM_INT4:
			len = size / sizeof(int);
			dInt.assign(ptrInt, ptrInt + len);
			break;
		}
		return dInt;
	}

	operator std::vector<bool>()
	{
		std::vector<float> dFloat;
		std::vector<int>   dInt;
		std::vector<bool>  dBool;
		float *            ptrFloat = static_cast<float *>(data);
		int *              ptrInt = static_cast<int *>(data);
		bool *             ptrBool = static_cast<bool *>(data);

		size_t i;
		size_t len;

		switch (type) {
		case GS_SHADER_PARAM_BOOL:
			len = size / sizeof(bool);
			dBool.assign(ptrBool, ptrBool + len);
			break;
		case GS_SHADER_PARAM_FLOAT:
		case GS_SHADER_PARAM_VEC2:
		case GS_SHADER_PARAM_VEC3:
		case GS_SHADER_PARAM_VEC4:
		case GS_SHADER_PARAM_MATRIX4X4:
			len = size / sizeof(float);
			dFloat.assign(ptrFloat, ptrFloat + len);
			dBool.reserve(len);
			for (i = 0; i < dFloat.size(); i++)
				dBool.push_back(dFloat[i]);
			break;
		case GS_SHADER_PARAM_INT:
		case GS_SHADER_PARAM_INT2:
		case GS_SHADER_PARAM_INT3:
		case GS_SHADER_PARAM_INT4:
			len = size / sizeof(int);
			dInt.assign(ptrInt, ptrInt + len);
			dBool.reserve(len);
			for (i = 0; i < dInt.size(); i++)
				dBool.push_back(dInt[i]);
			break;
		}
		return dBool;
	}

	operator std::string()
	{
		std::string str = "";
		char *      ptrChar = static_cast<char *>(data);

		switch (type) {
		case GS_SHADER_PARAM_STRING:
			str = ptrChar;
			break;
		}
		return str;
	}

	std::string getString()
	{
		return *this;
	}

	const char *c_str()
	{
		return ((std::string) * this).c_str();
	}
};

class EParam {
private:
	EVal *getValue(gs_eparam_t *eparam)
	{
		EVal *v = nullptr;

		if (eparam) {
			gs_effect_param_info note_info;
			gs_effect_get_param_info(eparam, &note_info);

			v = new EVal();
			v->data = gs_effect_get_default_val(eparam);
			v->size = gs_effect_get_default_val_size(eparam);
			v->type = note_info.type;
		}

		return v;
	}

protected:
	gs_eparam_t *                             _param = nullptr;
	gs_effect_param_info                      _paramInfo = { 0 };
	EVal *                                    _value = nullptr;
	std::unordered_map<std::string, EParam *> _annotationsMap;
	size_t                                    _annotationCount;

public:
	std::unordered_map<std::string, EParam *> *getAnnootations()
	{
		return &_annotationsMap;
	}

	gs_effect_param_info info() const
	{
		return _paramInfo;
	}

	EVal *getValue()
	{
		return _value ? _value : (_value = getValue(_param));
	}

	gs_eparam_t *getParam()
	{
		return _param;
	}

	operator gs_eparam_t *()
	{
		return _param;
	}

	size_t getAnnotationCount()
	{
		return _annotationsMap.size();
	}

	/* Hash Map Search */
	EParam *getAnnotation(std::string name)
	{
		if (_annotationsMap.find(name) != _annotationsMap.end())
			return _annotationsMap.at(name);
		else
			return nullptr;
	}

	EParam *operator[](std::string name)
	{
		return getAnnotation(name);
	}

	EVal *getAnnotationValue(std::string name)
	{
		EParam *note = getAnnotation(name);
		if (note)
			return note->getValue();
		else
			return nullptr;
	}

	template<class DataType> std::vector<DataType> getAnnotationValue(std::string name)
	{
		std::vector<DataType> ret;
		EParam *              note = getAnnotation(name);
		if (note)
			ret = *(note->getValue());
		return ret;
	}

	template<class DataType> DataType getAnnotationValue(std::string name, DataType defaultValue, int index = 0)
	{
		std::vector<DataType> ret;
		EParam *              note = getAnnotation(name);
		if (note)
			ret = *(note->getValue());
		if (index < ret.size())
			return ret[index];
		else
			return defaultValue;
	}

	bool hasAnnotation(std::string name)
	{
		return _annotationsMap.find(name) != _annotationsMap.end();
	}

	EParam(gs_eparam_t *param)
	{
		_param = param;
		gs_effect_get_param_info(param, &_paramInfo);
		_value = getValue(param);

		size_t i;
		_annotationCount = gs_param_get_num_annotations(_param);
		_annotationsMap.reserve(_annotationCount);

		gs_eparam_t *p = nullptr;

		for (i = 0; i < _annotationCount; i++) {
			p = gs_param_get_annotation_by_idx(_param, i);
			EParam *             ep = new EParam(p);
			gs_effect_param_info _info;
			gs_effect_get_param_info(p, &_info);

			_annotationsMap.insert(std::pair<std::string, EParam *>(_info.name, ep));
		}
	}

	~EParam()
	{
		if (_value)
			delete _value;
		for (const auto &annotation : _annotationsMap)
			delete annotation.second;
		_annotationsMap.clear();
	}

	template<class DataType> void setValue(DataType *data, size_t size)
	{
		size_t len = size / sizeof(DataType);
		size_t arraySize = len * sizeof(DataType);
		gs_effect_set_val(_param, data, arraySize);
	}

	template<class DataType> void setValue(std::vector<DataType> data)
	{
		size_t arraySize = data.size() * sizeof(DataType);
		gs_effect_set_val(_param, data.data(), arraySize);
	}
};

class ShaderData {
protected:
	gs_shader_param_type _paramType;

	ShaderSource *   _filter;
	ShaderParameter *_parent;
	EParam *         _param;

	std::vector<out_shader_data> _values;
	std::vector<in_shader_data>  _bindings;

	std::vector<std::string> _names;
	std::vector<std::string> _descs;
	std::vector<std::string> _tooltips;
	std::vector<std::string> _bindingNames;
	std::vector<std::string> _expressions;

	size_t _dataCount;

public:
	gs_shader_param_type getParamType() const
	{
		return _paramType;
	}

	ShaderParameter *getParent()
	{
		return _parent;
	}

	ShaderData(ShaderParameter *parent = nullptr, ShaderSource *filter = nullptr) : _parent(parent), _filter(filter)
	{
		if (_parent)
			_param = _parent->getParameter();
		else
			_param = nullptr;
	}

	virtual ~ShaderData()
	{
	};

	virtual void init(gs_shader_param_type paramType)
	{
		_paramType = paramType;
		_dataCount = getDataSize(paramType);

		_names.reserve(_dataCount);
		_descs.reserve(_dataCount);
		_values.reserve(_dataCount);
		_bindings.reserve(_dataCount);
		_expressions.reserve(_dataCount);
		_bindingNames.reserve(_dataCount);
		_tooltips.reserve(_dataCount);

		size_t          i;
		out_shader_data empty = { 0 };
		in_shader_data  emptyBinding = { 0 };

		std::string n = _parent->getName();
		std::string d = _parent->getDescription();
		std::string strNum = "";
		EVal *      val = nullptr;
		for (i = 0; i < _dataCount; i++) {
			if (_dataCount > 1)
				strNum = "_" + std::to_string(i);
			_names.push_back(n + strNum);
			val = _param->getAnnotationValue("desc" + strNum);
			if (val)
				_descs.push_back(*val);
			else
				_descs.push_back(d + strNum);
			_bindingNames.push_back(toSnakeCase(_names[i]));
			val = _param->getAnnotationValue("tooltiop" + strNum);
			if (val)
				_tooltips.push_back(*val);
			else
				_tooltips.push_back(_bindingNames[i]);
			_values.push_back(empty);
			_bindings.push_back(emptyBinding);

			val = _param->getAnnotationValue("expr" + strNum);
			if (val)
				_expressions.push_back(*val);
			else
				_expressions.push_back("");
		}

		for (i = 0; i < 4; i++) {
			if (_filter->resizeExpressions[i].empty()) {
				val = _param->getAnnotationValue("resize_expr_" + dir[i]);
				if (val)
					_filter->resizeExpressions[i] = val->getString();
			}
		}
	};

	virtual void getProperties(ShaderSource *filter, obs_properties_t *props)
	{
		UNUSED_PARAMETER(filter);
		UNUSED_PARAMETER(props);
	};

	virtual void videoTick(ShaderSource *filter, float elapsedTime, float seconds)
	{
		UNUSED_PARAMETER(filter);
		UNUSED_PARAMETER(elapsedTime);
		UNUSED_PARAMETER(seconds);
	};

	virtual void videoRender(ShaderSource *filter)
	{
		UNUSED_PARAMETER(filter);
	};

	virtual void update(ShaderSource *filter)
	{
		UNUSED_PARAMETER(filter);
	};

	virtual void onPass(ShaderSource *filter, const char *technique, size_t pass, gs_texture_t *texture)
	{
		UNUSED_PARAMETER(filter);
		UNUSED_PARAMETER(technique);
		UNUSED_PARAMETER(pass);
		UNUSED_PARAMETER(texture);
	}

	virtual void onTechniqueEnd(ShaderSource *filter, const char *technique, gs_texture_t *texture)
	{
		UNUSED_PARAMETER(filter);
		UNUSED_PARAMETER(technique);
		UNUSED_PARAMETER(texture);
	}
};

class NumericalData : public ShaderData {
private:
	void fillIntList(EParam *e, obs_property_t *p)
	{
		std::unordered_map<std::string, EParam *> *notations = e->getAnnootations();
		for (std::unordered_map<std::string, EParam *>::iterator it = notations->begin();
			it != notations->end(); it++) {
			EParam *    eparam = (*it).second;
			EVal *      eval = eparam->getValue();
			std::string name = eparam->info().name;

			if (name.compare(0, 9, "list_item") == 0 && name.compare(name.size() - 6, 5, "_name") != 0) {
				std::vector<int> iList = *eval;
				if (iList.size()) {
					EVal *      evalname = e->getAnnotationValue((name + "_name"));
					std::string itemname = *evalname;
					int         d = iList[0];
					if (itemname.empty())
						itemname = std::to_string(d);
					obs_property_list_add_int(p, itemname.c_str(), d);
				}
			}
		}
	}

	void fillFloatList(EParam *e, obs_property_t *p)
	{
		std::unordered_map<std::string, EParam *> *notations = e->getAnnootations();
		for (std::unordered_map<std::string, EParam *>::iterator it = notations->begin();
			it != notations->end(); it++) {
			EParam *    eparam = (*it).second;
			EVal *      eval = eparam->getValue();
			std::string name = eparam->info().name;

			if (name.compare(0, 9, "list_item") == 0 && name.compare(name.size() - 6, 5, "_name") != 0) {
				std::vector<float> fList = *eval;
				if (fList.size()) {
					EVal *      evalname = e->getAnnotationValue((name + "_name"));
					std::string itemname = *evalname;
					double      d = fList[0];
					if (itemname.empty())
						itemname = std::to_string(d);
					obs_property_list_add_float(p, itemname.c_str(), d);
				}
			}
		}
	}

	void fillComboBox(EParam *e, obs_property_t *p)
	{
		EVal *      enabledval = e->getAnnotationValue("enabled_desc");
		EVal *      disabledval = e->getAnnotationValue("disabled_desc");
		std::string enabled = _OMT("On");
		std::string disabled = _OMT("Off");
		if (enabledval) {
			std::string temp = *enabledval;
			if (!temp.empty())
				enabled = temp;
		}
		if (disabledval) {
			std::string temp = *disabledval;
			if (!temp.empty())
				disabled = temp;
		}
		obs_property_list_add_int(p, enabled.c_str(), 1);
		obs_property_list_add_int(p, disabled.c_str(), 0);
	}

protected:
	bool              _isFloat;
	bool              _isInt;
	bool              _isSlider;
	bool              _skipWholeProperty;
	bool              _skipCalculations;
	bool              _showExpressionLess;
	std::vector<bool> _skipProperty;
	std::vector<bool> _disableProperty;
	double            _min;
	double            _max;
	double            _step;
	enum BindType {
		unspecified, none, byte, short_integer, integer, floating_point, double_point
	};
	void *   _bind = nullptr;
	BindType bindType;
	enum NumericalType {
		combobox, list, num, slider, color
	};

	NumericalType _numType;

public:
	NumericalData(ShaderParameter *parent, ShaderSource *filter) : ShaderData(parent, filter)
	{
		gs_eparam_t *               param = parent->getParameter()->getParam();
		struct gs_effect_param_info info;
		gs_effect_get_param_info(param, &info);
		/* std::vector<DataType> *bind */
		std::string n = info.name;
		if (n == "ViewProj") {
			bindType = floating_point;
			_bind = &_filter->viewProj;
		} else if (n == "uv_offset") {
			bindType = floating_point;
			_bind = &_filter->uvOffset;
		} else if (n == "uv_scale") {
			bindType = floating_point;
			_bind = &_filter->uvScale;
		} else if (n == "uv_pixel_interval") {
			bindType = floating_point;
			_bind = &_filter->uvPixelInterval;
		} else if (n == "elapsed_time") {
			bindType = floating_point;
			_bind = &_filter->elapsedTime;
		}
		if (_filter->getType() == OBS_SOURCE_TYPE_TRANSITION) {
			if (n == "transition_percentage") {
				bindType = floating_point;
				_bind = &_filter->transitionPercentage;
			} else if (n == "transition_time") {
				bindType = floating_point;
				_bind = &_filter->transitionSeconds;
			}
		}
	};

	~NumericalData()
	{
	};

	void init(gs_shader_param_type paramType)
	{
		ShaderData::init(paramType);
		_isFloat = isFloatType(paramType);
		_isInt = isIntType(paramType);
		_skipWholeProperty = _bind ? true : false;
		_skipCalculations = false;
		size_t i;
		if (_isFloat) {
			_min = (double)_param->getAnnotationValue<float>("min", -FLT_MAX);
			_max = (double)_param->getAnnotationValue<float>("max", FLT_MAX);
			_step = (double)_param->getAnnotationValue<float>("step", 1.0);
		} else {
			_min = (double)_param->getAnnotationValue<int>("min", INT_MIN);
			_max = (double)_param->getAnnotationValue<int>("max", INT_MAX);
			_step = (double)_param->getAnnotationValue<int>("step", 1);
		}

		EVal *guitype = _param->getAnnotationValue("type");
		bool  isSlider = _param->getAnnotationValue<bool>("is_slider", true);

		std::unordered_map<std::string, uint32_t> types = { {"combobox", combobox}, {"list", list}, {"num", num},
				{"slider", slider}, {"color", color} };

		_numType = num;
		if (guitype && types.find(guitype->getString()) != types.end()) {
			_numType = (NumericalType)types.at(guitype->getString());
		} else if (isSlider) {
			_numType = slider;
		}

		_disableProperty.reserve(_dataCount);
		_skipProperty.reserve(_dataCount);

		for (i = 0; i < _dataCount; i++) {
			if (_filter)
				_filter->appendVariable(_bindingNames[i], &_bindings[i].d);
		}

		bool hasExpressions = false;
		for (i = 0; i < _expressions.size(); i++) {
			if (_expressions[i].empty()) {
				_disableProperty.push_back(false);
				_skipProperty.push_back(false);
				continue;
			}
			hasExpressions = true;
			_filter->compileExpression(_expressions[i]);
			if (_filter->expressionCompiled()) {
				_disableProperty.push_back(false);
				_skipProperty.push_back(true);
			} else {
				_disableProperty.push_back(true);
				_skipProperty.push_back(false);
				_tooltips[i] = _filter->expressionError();
			}
		}

		bool showExprLess = _param->getAnnotationValue<bool>("show_exprless", false);
		if (!showExprLess)
			_showExpressionLess = !hasExpressions;
		else
			_showExpressionLess = showExprLess;
	}

	void getProperties(ShaderSource *filter, obs_properties_t *props)
	{
		UNUSED_PARAMETER(filter);
		size_t i;
		if (_bind || _skipWholeProperty)
			return;
		obs_property_t *p;
		if (_isFloat) {
			if (_numType == color && _dataCount == 4) {
				obs_properties_add_color(props, _names[0].c_str(), _descs[0].c_str());
				return;
			}
			for (i = 0; i < _dataCount; i++) {
				if (_skipProperty[i])
					continue;
				if (!_showExpressionLess && _expressions[i].empty())
					continue;
				switch (_numType) {
				case combobox:
				case list:
					p = obs_properties_add_list(props, _names[i].c_str(), _descs[i].c_str(),
						OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
					fillFloatList(_param, p);
					break;
				case slider:
					p = obs_properties_add_float_slider(
						props, _names[i].c_str(), _descs[i].c_str(), _min, _max, _step);
					break;
				default:
					p = obs_properties_add_float(
						props, _names[i].c_str(), _descs[i].c_str(), _min, _max, _step);
					break;
				}
				obs_property_set_enabled(p, !_disableProperty[i]);
				obs_property_set_long_description(p, _tooltips[i].c_str());
			}
		} else if (_isInt) {
			for (i = 0; i < _dataCount; i++) {
				if (_skipProperty[i])
					continue;
				if (!_showExpressionLess && _expressions[i].empty())
					continue;
				switch (_numType) {
				case combobox:
				case list:
					p = obs_properties_add_list(props, _names[i].c_str(), _descs[i].c_str(),
						OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
					fillIntList(_param, p);
					break;
				case slider:
					p = obs_properties_add_int_slider(props, _names[i].c_str(), _descs[i].c_str(),
						(int)_min, (int)_max, (int)_step);
					break;
				default:
					p = obs_properties_add_int(props, _names[i].c_str(), _descs[i].c_str(),
						(int)_min, (int)_max, (int)_step);
					break;
				}
				obs_property_set_enabled(p, !_disableProperty[i]);
				obs_property_set_long_description(p, _tooltips[i].c_str());
			}
		} else {
			for (i = 0; i < _dataCount; i++) {
				if (_skipProperty[i])
					continue;
				if (!_showExpressionLess && _expressions[i].empty())
					continue;
				switch (_numType) {
				case combobox:
				case list:
					p = obs_properties_add_list(props, _names[i].c_str(), _descs[i].c_str(),
						OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
					fillComboBox(_param, p);
					break;
				default:
					p = obs_properties_add_bool(props, _names[i].c_str(), _descs[i].c_str());
				}
				obs_property_set_enabled(p, !_disableProperty[i]);
				obs_property_set_long_description(p, _tooltips[i].c_str());
			}
		}
	}

	void update(ShaderSource *filter)
	{
		if (_bind || _skipWholeProperty)
			return;
		obs_data_t *settings = filter->getSettings();
		size_t      i;
		for (i = 0; i < _dataCount; i++) {
			switch (_paramType) {
			case GS_SHADER_PARAM_BOOL:
				switch (_numType) {
				case combobox:
				case list:
					_bindings[i].d = (double)obs_data_get_int(settings, _names[i].c_str());
					_values[i].s32i = (int32_t)_bindings[i].d;
					break;
				default:
					_bindings[i].d = (double)obs_data_get_bool(settings, _names[i].c_str());
					_values[i].s32i = (int32_t)_bindings[i].d;
					break;
				}
				break;
			case GS_SHADER_PARAM_INT:
			case GS_SHADER_PARAM_INT2:
			case GS_SHADER_PARAM_INT3:
			case GS_SHADER_PARAM_INT4:
				_bindings[i].d = (double)obs_data_get_int(settings, _names[i].c_str());
				_values[i].s32i = (int32_t)_bindings[i].d;
				break;
			case GS_SHADER_PARAM_FLOAT:
			case GS_SHADER_PARAM_VEC2:
			case GS_SHADER_PARAM_VEC3:
			case GS_SHADER_PARAM_VEC4:
			case GS_SHADER_PARAM_MATRIX4X4:
				_bindings[i].d = obs_data_get_double(settings, _names[i].c_str());
				_values[i].f = (float)_bindings[i].d;
				break;
			default:
				break;
			}
		}
	}

	void videoTick(ShaderSource *filter, float elapsedTime, float seconds)
	{
		UNUSED_PARAMETER(seconds);
		UNUSED_PARAMETER(elapsedTime);
		size_t i;
		if (_skipCalculations)
			return;
		for (i = 0; i < _dataCount; i++) {
			if (!_expressions[i].empty()) {
				switch (_paramType) {
				case GS_SHADER_PARAM_BOOL:
					_filter->compileExpression(_expressions[i]);
					_bindings[i].d = (double)filter->evaluateExpression<long long>(0);
					_values[i].s32i = (int32_t)_bindings[i].d;
					break;
				case GS_SHADER_PARAM_INT:
				case GS_SHADER_PARAM_INT2:
				case GS_SHADER_PARAM_INT3:
				case GS_SHADER_PARAM_INT4:
					_filter->compileExpression(_expressions[i]);
					_bindings[i].d = (double)filter->evaluateExpression<long long>(0);
					_values[i].s32i = (int32_t)_bindings[i].d;
					break;
				case GS_SHADER_PARAM_FLOAT:
				case GS_SHADER_PARAM_VEC2:
				case GS_SHADER_PARAM_VEC3:
				case GS_SHADER_PARAM_VEC4:
				case GS_SHADER_PARAM_MATRIX4X4:
					_filter->compileExpression(_expressions[i]);
					_bindings[i].d = (double)filter->evaluateExpression<double>(0);
					_values[i].f = (float)_bindings[i].d;
					break;
				default:
					break;
				}
			} else if (_bind) {
				switch (_paramType) {
				case GS_SHADER_PARAM_BOOL:
					_bindings[i].d = (double)static_cast<bool *>(_bind)[i];
					_values[i].s32i = (int32_t)_bindings[i].d;
					break;
				case GS_SHADER_PARAM_INT:
				case GS_SHADER_PARAM_INT2:
				case GS_SHADER_PARAM_INT3:
				case GS_SHADER_PARAM_INT4:
					_bindings[i].d = (double)static_cast<int *>(_bind)[i];
					_values[i].s32i = (int32_t)_bindings[i].d;
					break;
				case GS_SHADER_PARAM_FLOAT:
				case GS_SHADER_PARAM_VEC2:
				case GS_SHADER_PARAM_VEC3:
				case GS_SHADER_PARAM_VEC4:
				case GS_SHADER_PARAM_MATRIX4X4:
					_bindings[i].d = (double)static_cast<float *>(_bind)[i];
					_values[i].f = (float)_bindings[i].d;
					break;
				default:
					break;
				}
			}
		}
	}

	void setData()
	{
		if (_param) {
			if (_isFloat) {
				float *data = (float *)_values.data();
				_param->setValue<float>(data, _values.size() * sizeof(float));
			} else {
				int *data = (int *)_values.data();
				_param->setValue<int>(data, _values.size() * sizeof(int));
			}
		}
	}

	template<class DataType> void setData(DataType t)
	{
		if (_param)
			_param->setValue<DataType>(&t, sizeof(t));
	}

	template<class DataType> void setData(std::vector<DataType> t)
	{
		if (_param)
			_param->setValue<DataType>(t.data(), t.size() * sizeof(DataType));
	}

	void videoRender(ShaderSource *filter)
	{
		UNUSED_PARAMETER(filter);
		if (_skipCalculations)
			return;

		setData();
	}
};

/* TODO? */
class StringData : public ShaderData {
	std::string _value;

	std::vector<std::string> _binding;
	std::vector<double>      _bindings;

public:
	StringData(ShaderParameter *parent, ShaderSource *filter) : ShaderData(parent, filter)
	{
	};

	~StringData()
	{
	};

	void init(gs_shader_param_type paramType)
	{
		ShaderData::init(paramType);
	}
};

/* functions to add sources to a list for use as textures */
static bool fillPropertiesSourceList(void *param, obs_source_t *source)
{
	obs_property_t *p = (obs_property_t *)param;
	uint32_t        flags = obs_source_get_output_flags(source);
	const char *    sourceName = obs_source_get_name(source);

	if ((flags & OBS_SOURCE_VIDEO) != 0 && obs_source_active(source))
		obs_property_list_add_string(p, sourceName, sourceName);

	return true;
}

static void fillSourceList(obs_property_t *p)
{
	obs_property_list_add_string(p, _OMT("None"), "");
	obs_enum_sources(&fillPropertiesSourceList, (void *)p);
}

static bool fillPropertiesAudioSourceList(void *param, obs_source_t *source)
{
	obs_property_t *p = (obs_property_t *)param;
	uint32_t        flags = obs_source_get_output_flags(source);
	const char *    sourceName = obs_source_get_name(source);

	if ((flags & OBS_SOURCE_AUDIO) != 0 && obs_source_active(source))
		obs_property_list_add_string(p, sourceName, sourceName);

	return true;
}

static void fillAudioSourceList(obs_property_t *p)
{
	obs_property_list_add_string(p, _OMT("None"), "");
	obs_enum_sources(&fillPropertiesAudioSourceList, (void *)p);
}

class TextureData : public ShaderData {
private:
	void renderSource(EParam *param, uint32_t cx, uint32_t cy)
	{
		if (!param)
			return;
		uint32_t mediaWidth = obs_source_get_width(_mediaSource);
		uint32_t mediaHeight = obs_source_get_height(_mediaSource);

		if (!mediaWidth || !mediaHeight)
			return;

		_sourceWidth = mediaWidth;
		_sourceHeight = mediaHeight;

		float scale_x = cx / (float)mediaWidth;
		float scale_y = cy / (float)mediaHeight;

		gs_texrender_reset(_texrender);
		if (gs_texrender_begin(_texrender, mediaWidth, mediaHeight)) {
			struct vec4 clearColor;
			vec4_zero(&clearColor);

			gs_clear(GS_CLEAR_COLOR, &clearColor, 1, 0);
			gs_matrix_scale3f(scale_x, scale_y, 1.0f);
			obs_source_video_render(_mediaSource);

			gs_texrender_end(_texrender);
		} else {
			return;
		}

		//gs_texture_t *tex = gs_texrender_get_texture(_texrender);
		//gs_effect_set_texture(*param, tex);
	}

	uint32_t processAudio(size_t samples)
	{
		size_t i;
		size_t hSamples = samples / 2;
		size_t hSamplesSize = samples * 2;

		for (i = 0; i < _channels; i++)
			audio_fft_complex(((float *)_data) + (i * samples), (uint32_t)samples);
		for (i = 1; i < _channels; i++)
			memcpy(((float *)_data) + (i * hSamples), ((float *)_data) + (i * samples), hSamplesSize);
		return (uint32_t)hSamples;
	}

	void renderAudioSource(EParam *param, uint64_t samples)
	{
		if (!_data)
			_data = (uint8_t *)bzalloc(_maxAudioSize * _channels * sizeof(float));
		size_t pxWidth = samples;
		audiolock();
		size_t i = 0;
		for (i = 0; i < _channels; i++) {
			if (_audio[i].data())
				memcpy((float *)_data + (samples * i), _audio[i].data(), samples * sizeof(float));
			else
				memset((float *)_data + (samples * i), 0, samples * sizeof(float));
		}
		audiounlock();

		if (_isFFT)
			pxWidth = processAudio(samples);

		_sourceWidth = (double)pxWidth;
		_sourceHeight = (double)_channels;
		obs_enter_graphics();
		gs_texture_destroy(_tex);
		_tex = gs_texture_create(
			(uint32_t)pxWidth, (uint32_t)_channels, GS_R32F, 1, (const uint8_t **)&_data, 0);
		obs_leave_graphics();
		//gs_effect_set_texture(*param, _tex);
	}

	void updateAudioSource(std::string name)
	{
		if (!name.empty()) {
			obs_source_t *sideChain = nullptr;
			sideChain = obs_get_source_by_name(name.c_str());
			obs_source_t *oldSideChain = _mediaSource;
			lock();
			if (oldSideChain) {
				obs_source_remove_audio_capture_callback(oldSideChain, sidechain_capture, this);
				obs_source_release(oldSideChain);
				for (size_t i = 0; i < MAX_AV_PLANES; i++)
					_audio[i].clear();
			}
			if (sideChain)
				obs_source_add_audio_capture_callback(sideChain, sidechain_capture, this);
			_mediaSource = sideChain;
			unlock();
		}
	}

	PThreadMutex *_mutex = nullptr;
	PThreadMutex *_audioMutex = nullptr;

protected:
	gs_texrender_t *   _texrender = nullptr;
	gs_texture_t *     _tex = nullptr;
	gs_image_file_t *  _image = nullptr;
	std::vector<float> _audio[MAX_AV_PLANES];
	std::vector<float> _tempAudio[MAX_AV_PLANES];
	bool               _isFFT = false;
	bool               _isParticle = false;
	std::vector<float> _fft_data[MAX_AV_PLANES];
	size_t             _channels = 0;
	size_t             _maxAudioSize = AUDIO_OUTPUT_FRAMES * 2;
	uint8_t *          _data = nullptr;
	obs_source_t *     _mediaSource = nullptr;
	std::string        _sourceName = "";
	size_t             _size;
	uint8_t            _range_0;
	uint8_t            _range_1;
	enum TextureType {
		ignored, unspecified, source, audio, image, media, random, buffer
	};
	fft_windowing_type _window;
	TextureType        _texType;
	std::string        _filePath;

	std::string _sizeWBinding;
	std::string _sizeHBinding;
	std::string _mediaSourceLengthBinding;
	std::string _mediaSourceFramesBinding;
	std::string _tech;
	size_t      _pass;
	double      _sourceWidth;
	double      _sourceHeight;
	double      _mediaSourceLength;
	double      _mediaSourceFrames;

	struct transformAlpha {
		matrix4 position;

		float rotateX;
		float rotateY;
		float rotateZ;

		float translateX;
		float translateY;
		float translateZ;

		//matrix4 m;
		float f;
	};
	gs_texrender_t * _particlerender = nullptr;
	std::vector<transformAlpha> _particles;
public:
	TextureData(ShaderParameter *parent, ShaderSource *filter)
		: ShaderData(parent, filter), _maxAudioSize(AUDIO_OUTPUT_FRAMES * 2)
	{
		_maxAudioSize = AUDIO_OUTPUT_FRAMES * 2;
		_mutex = new PThreadMutex();
		_audioMutex = new PThreadMutex();
	};

	~TextureData()
	{
		if (_texType == audio)
			obs_source_remove_audio_capture_callback(_mediaSource, sidechain_capture, this);
		if (_mediaSource)
			obs_source_release(_mediaSource);
		_mediaSource = nullptr;

		obs_enter_graphics();
		gs_texrender_destroy(_texrender);
		gs_image_file_free(_image);
		if (_tex)
			gs_texture_destroy(_tex);
		obs_leave_graphics();
		_texrender = nullptr;
		_tex = nullptr;
		if (_image)
			bfree(_image);
		_image = nullptr;

		if (_data)
			bfree(_data);
		if (_mutex)
			delete _mutex;
		if (_audioMutex)
			delete _audioMutex;
	};

	void lock()
	{
		_mutex->lock();
	}

	void unlock()
	{
		_mutex->unlock();
	}

	void audiolock()
	{
		_audioMutex->lock();
	}

	void audiounlock()
	{
		_audioMutex->unlock();
	}

	size_t getAudioChannels()
	{
		return _channels;
	}

	void insertAudio(float *data, size_t samples, size_t index)
	{
		if (!samples || index > (MAX_AV_PLANES - 1))
			return;
		audiolock();
		size_t oldSize = _audio[index].size() * sizeof(float);
		size_t insertSize = samples * sizeof(float);
		float *oldData = nullptr;
		if (oldSize)
			oldData = (float *)bmemdup(_audio[index].data(), oldSize);
		_audio[index].resize(_maxAudioSize);
		if (samples < _maxAudioSize) {
			if (oldData)
				memcpy(&_audio[index][samples], oldData, oldSize - insertSize);
			if (data)
				memcpy(&_audio[index][0], data, insertSize);
			else
				memset(&_audio[index][0], 0, insertSize);
		} else {
			if (data)
				memcpy(&_audio[index][0], data, _maxAudioSize * sizeof(float));
			else
				memset(&_audio[index][0], 0, _maxAudioSize * sizeof(float));
		}
		audiounlock();
		bfree(oldData);
	}

	void init(gs_shader_param_type paramType)
	{
		_paramType = paramType;
		_names.push_back(_parent->getName());
		_descs.push_back(_parent->getDescription());

		EVal *                                    texType = _param->getAnnotationValue("type");
		std::unordered_map<std::string, uint32_t> types = { {"source", source}, {"audio", audio},
				{"image", image}, {"media", media}, {"random", random}, {"buffer", buffer} };

		if (texType && types.find(texType->getString()) != types.end())
			_texType = (TextureType)types.at(texType->getString());
		else
			_texType = image;

		if (_names[0] == "image")
			_texType = ignored;
		else if (_filter->getType() == OBS_SOURCE_TYPE_TRANSITION && _names[0] == "image_2")
			_texType = ignored;

		_channels = audio_output_get_channels(obs_get_audio());
		_bindingNames.push_back(toSnakeCase(_names[0]));

		EVal *techAnnotation = _param->getAnnotationValue("technique");
		EVal *window = nullptr;
		switch (_texType) {
		case audio:
			_channels = _param->getAnnotationValue<int>("channels", 0);

			for (size_t i = 0; i < MAX_AV_PLANES; i++)
				_audio->resize(AUDIO_OUTPUT_FRAMES);

			_isFFT = _param->getAnnotationValue<bool>("is_fft", false);

			window = _param->getAnnotationValue("window");
			if (window)
				_window = get_window_type(window->c_str());
			else
				_window = none;
			break;
		case buffer:
			if (techAnnotation)
				_tech = techAnnotation->getString();
			else
				_tech = "";
			_pass = _param->getAnnotationValue<int>("pass", -1);
			break;
		case media:
			_mediaSourceFramesBinding = _bindingNames[0] + "_frames";
			_mediaSourceLengthBinding = _bindingNames[0] + "_sec";

			if (_filter) {
				_filter->appendVariable(_mediaSourceFramesBinding, &_mediaSourceFrames);
				_filter->appendVariable(_mediaSourceLengthBinding, &_mediaSourceLength);
			}
			break;
		default:
			break;
		}

		_sizeWBinding = _bindingNames[0] + "_w";
		_sizeHBinding = _bindingNames[0] + "_h";

		if (_filter) {
			_filter->appendVariable(_sizeWBinding, &_sourceWidth);
			_filter->appendVariable(_sizeHBinding, &_sourceHeight);
		}
	}

	void getProperties(ShaderSource *filter, obs_properties_t *props)
	{
		UNUSED_PARAMETER(filter);
		obs_property_t *p = nullptr;
		switch (_texType) {
		case source:
			p = obs_properties_add_list(props, _names[0].c_str(), _descs[0].c_str(), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);
			fillSourceList(p);
			for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
				std::string l = obs_property_list_item_string(p, i);
				std::string src = obs_source_get_name(_filter->context);
				if (l == src)
					obs_property_list_item_remove(p, i--);
				obs_source_t *parent = obs_filter_get_parent(_filter->context);
				std::string   parentName = "";
				if (parent)
					parentName = obs_source_get_name(parent);
				if (!parentName.empty() && l == parentName)
					obs_property_list_item_remove(p, i--);
			}
			break;
		case audio:
			p = obs_properties_add_list(props, _names[0].c_str(), _descs[0].c_str(), OBS_COMBO_TYPE_LIST,
				OBS_COMBO_FORMAT_STRING);
			fillAudioSourceList(p);
			for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
				std::string l = obs_property_list_item_string(p, i);
				std::string src = obs_source_get_name(_filter->context);
				if (l == src)
					obs_property_list_item_remove(p, i--);
				obs_source_t *parent = obs_filter_get_parent(_filter->context);
				std::string   parentName = "";
				if (parent)
					parentName = obs_source_get_name(parent);
				if (!parentName.empty() && l == parentName)
					obs_property_list_item_remove(p, i--);
			}
			break;
		case media:
			p = obs_properties_add_path(props, _names[0].c_str(), _descs[0].c_str(), OBS_PATH_FILE,
				shader_filter_media_file_filter, NULL);
			break;
		case image:
			p = obs_properties_add_path(props, _names[0].c_str(), _descs[0].c_str(), OBS_PATH_FILE,
				shader_filter_texture_file_filter, NULL);
			break;
		case random:
			obs_properties_add_int(props, (_names[0] + "_range_0").c_str(), _descs[0].c_str(), 0, 255, 1);
			obs_properties_add_int(props, (_names[0] + "_range_1").c_str(), _descs[0].c_str(), 0, 255, 1);
			break;
		}
	}

	void update(ShaderSource *filter)
	{
		obs_data_t *settings = filter->getSettings();
		_channels = audio_output_get_channels(obs_get_audio());
		const char *filePath;
		proc_handler_t *ph = nullptr;
		obs_data_t *media_settings = nullptr;
		const char *path;
		calldata_t cd = { 0 };

		switch (_texType) {
		case source:
			if (!_texrender)
				_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			obs_source_release(_mediaSource);
			_mediaSource = obs_get_source_by_name(obs_data_get_string(settings, _names[0].c_str()));
			break;
		case media:
			if (!_texrender)
				_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

			path = obs_data_get_string(settings, _names[0].c_str());
			media_settings = obs_data_create();
			obs_data_set_string(media_settings, "local_file", path);

			obs_source_release(_mediaSource);
			_mediaSource = obs_source_create_private("ffmpeg_source", NULL, media_settings);

			obs_data_release(media_settings);

			ph = obs_source_get_proc_handler(_mediaSource);

			proc_handler_call(ph, "get_duration", &cd);
			proc_handler_call(ph, "get_nb_frames", &cd);

			_mediaSourceLength = ((uint64_t)calldata_int(&cd, "duration") / 1000000000.0);
			_mediaSourceFrames = (uint64_t)calldata_int(&cd, "num_frames");


			break;
		case audio:
			updateAudioSource(obs_data_get_string(settings, _names[0].c_str()));
			break;
		case image:
			if (!_image) {
				_image = (gs_image_file_t *)bzalloc(sizeof(gs_image_file_t));
			} else {
				obs_enter_graphics();
				gs_image_file_free(_image);
				obs_leave_graphics();
			}

			filePath = obs_data_get_string(settings, _names[0].c_str());
			_filePath = filePath;
			if (filePath && filePath[0] != '\0') {
				gs_image_file_init(_image, filePath);
				obs_enter_graphics();
				gs_image_file_init_texture(_image);
				obs_leave_graphics();
			}
			break;
		case random:
			_range_0 = (uint8_t)obs_data_get_int(settings, (_names[0] + "_range_0").c_str());
			_range_1 = (uint8_t)obs_data_get_int(settings, (_names[0] + "_range_1").c_str());
			break;
		}
	}

	void videoTick(ShaderSource *filter, float elapsedTime, float seconds)
	{
		UNUSED_PARAMETER(seconds);
		UNUSED_PARAMETER(elapsedTime);
		gs_texture_t *t;
		obs_enter_graphics();
		switch (_texType) {
		case media:
		case source:
			break;
		case audio:
			break;
		case image:
			t = _image ? _image->texture : NULL;
			if (t) {
				_sourceWidth = gs_texture_get_height(t);
				_sourceHeight = gs_texture_get_width(t);
			} else {
				_sourceWidth = 0;
				_sourceHeight = 0;
			}
			break;
		case random:
		case ignored:
			_sourceWidth = obs_source_get_width(filter->context);
			_sourceHeight = obs_source_get_height(filter->context);
			break;
		default:
			break;
		}
		obs_leave_graphics();
	}

	void videoRender(ShaderSource *filter)
	{
		ShaderData::videoRender(filter);
		uint32_t      srcWidth = obs_source_get_width(filter->context);
		uint32_t      srcHeight = obs_source_get_height(filter->context);
		gs_texture_t *t = nullptr;
		size_t        pixels;
		size_t        i;
		uint8_t       u;
		switch (_texType) {
		case media:
		case source:
			renderSource(_param, srcWidth, srcHeight);
			t = gs_texrender_get_texture(_texrender);
			break;
		case audio:
			renderAudioSource(_param, AUDIO_OUTPUT_FRAMES);
			t = _tex;
			break;
		case image:
			t = _image ? _image->texture : NULL;

			//if (_param)
			//	_param->setValue<gs_texture_t *>(&t, sizeof(gs_texture_t *));
			break;
		case random:
			pixels = srcHeight * srcWidth;
			if (!_data)
				_data = (uint8_t *)bmalloc(pixels);

			if (_range_0 < _range_1) {
				for (i = 0; i < pixels; i++)
					_data[i] = (uint8_t)random_int(_range_0, _range_1);
			} else {
				for (i = 0; i < pixels; i++) {
					u = (uint8_t)random_int(0, _range_1 + (255 - _range_0));
					if (u > _range_1)
						u += _range_1 - _range_0;
					_data[i] = u;
				}
			}

			obs_enter_graphics();
			gs_texture_destroy(_tex);
			_tex = gs_texture_create(
				(uint32_t)srcWidth, (uint32_t)srcHeight, GS_R8, 1, (const uint8_t **)&_data, 0);
			obs_leave_graphics();
			t = _tex;
			//_param->setValue<gs_texture_t *>(&_tex, sizeof(gs_texture_t *));
			break;
		case buffer:
			t = _tex;
			//_param->setValue<gs_texture_t *>(&_tex, sizeof(gs_texture_t *));
			break;
		default:
			break;
		}
#define DEBUG_PARTICLES
#ifdef DEBUG_PARTICLES
		_isParticle = true;
		if (_particles.size() == 0) {
			transformAlpha p = {0};
			matrix4_identity(&p.position);
			p.rotateX = 0.01;
			//p.translateX = 1;
			_particles.push_back(p);
		}
#endif // DEBUG_PARTICLES
		if (_isParticle) {
			if (!_particlerender)
				_particlerender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
			gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_texrender_reset(_particlerender);
			// Allocate memory for data.
			size_t vCap = 4;
			gs_vb_data *m_vertexbufferdata = gs_vbdata_create();
			m_vertexbufferdata->num = vCap;
			m_vertexbufferdata->points = (vec3*)bmalloc(sizeof(vec3) * m_vertexbufferdata->num);
			m_vertexbufferdata->normals = (vec3*)bmalloc(sizeof(vec3) * m_vertexbufferdata->num);
			m_vertexbufferdata->tangents = (vec3*)bmalloc(sizeof(vec3) * m_vertexbufferdata->num);
			m_vertexbufferdata->colors = (uint32_t*)bmalloc(sizeof(uint32_t) * m_vertexbufferdata->num);
			m_vertexbufferdata->tvarray = (gs_tvertarray*)bmalloc(sizeof(gs_tvertarray));
			m_vertexbufferdata->tvarray->array = (vec4*)bmalloc(sizeof(vec4) * m_vertexbufferdata->num);
			m_vertexbufferdata->tvarray->width = 4;//m_vertexbufferdata->num;
			//m_vertexbufferdata->tvarray = (vec4*)bmalloc(sizeof(vec3) * m_vertexbufferdata->num);
			gs_vertbuffer_t *vbuf = gs_vertexbuffer_create(m_vertexbufferdata, GS_DYNAMIC);

			if (gs_texrender_begin(_particlerender, _filter->totalWidth, _filter->totalHeight)) {
				/*
				gs_set_cull_mode(GS_NEITHER);
				gs_enable_blending(false);
				gs_enable_depth_test(false);
				gs_depth_function(gs_depth_test::GS_ALWAYS);
				gs_enable_stencil_test(false);
				gs_enable_stencil_write(false);
				gs_enable_color(true, true, true, true);
				*/
				/*
				if (is_orthographic) {
					gs_ortho(-1.0, 1.0, -1.0, 1.0, -farZ, farZ);
				} else {
					gs_perspective(field_of_view, float_t(_filter->totalWidth) / float_t(_filter->totalHeight), nearZ, farZ);
					// Fix camera pointing at -Z instead of +Z.
					gs_matrix_scale3f(1.0, 1.0, -1.0);
					// Move backwards so we can actually see stuff.
					gs_matrix_translate3f(0, 0, 1.0);
				}
				*/
				gs_set_cull_mode(GS_NEITHER);
				//gs_enable_blending(false);
				gs_enable_depth_test(false);
				gs_depth_function(gs_depth_test::GS_ALWAYS);
				gs_ortho(-1.0, 1.0, -1.0, 1.0, -farZ, farZ);
				//gs_ortho(0, 1.0, 0, 1.0, -farZ, farZ);
				//gs_enable_stencil_test(false);
				//gs_enable_stencil_write(false);
				gs_enable_color(true, true, true, true);

				struct vec4 clearColor;
				vec4_zero(&clearColor);

				gs_clear(GS_CLEAR_COLOR | GS_CLEAR_DEPTH, &clearColor, farZ, 0);

				for (i = 0; i < _particles.size(); i++) {
					transformAlpha *p = &_particles[i];
					//rotate matrix
					matrix4_rotate_aa4f(&p->position, &p->position, 1, 0, 0, p->rotateX);
					matrix4_rotate_aa4f(&p->position, &p->position, 0, 1, 0, p->rotateY);
					matrix4_rotate_aa4f(&p->position, &p->position, 0, 0, 1, p->rotateZ);
					//transalte matrix
					matrix4_translate3f(&p->position, &p->position, p->translateX, p->translateY, p->translateZ);

					vec4 *ar = (vec4 *)m_vertexbufferdata->tvarray->array;
					m_vertexbufferdata->colors[0] = 0xFFFFFFFF;
					m_vertexbufferdata->colors[1] = 0xFFFFFFFF;
					m_vertexbufferdata->colors[2] = 0xFFFFFFFF;
					m_vertexbufferdata->colors[3] = 0xFFFFFFFF;

					vec4_set(&ar[0], 0, 0, 0, 0);
					vec4_set(&ar[1], 1, 0, 0, 0);
					vec4_set(&ar[2], 0, 1, 0, 0);
					vec4_set(&ar[3], 1, 1, 0, 0);
					/*
					uint32_t w = gs_texture_get_width(t);
					uint32_t h = gs_texture_get_height(t);
					*/
					uint32_t w = 1;
					uint32_t h = 1;
					vec3_set(&m_vertexbufferdata->points[0], w / -2.0, h / -2.0, 0);
					vec3_set(&m_vertexbufferdata->points[1], w / 2.0, h / -2.0, 0);
					vec3_set(&m_vertexbufferdata->points[2], w / -2.0, h / 2.0, 0);
					vec3_set(&m_vertexbufferdata->points[3], w / 2.0, h / 2.0, 0);

					vec3_transform(&m_vertexbufferdata->points[0], &m_vertexbufferdata->points[0], &p->position);
					vec3_transform(&m_vertexbufferdata->points[1], &m_vertexbufferdata->points[1], &p->position);
					vec3_transform(&m_vertexbufferdata->points[2], &m_vertexbufferdata->points[2], &p->position);
					vec3_transform(&m_vertexbufferdata->points[3], &m_vertexbufferdata->points[3], &p->position);

					gs_load_vertexbuffer(vbuf);
					gs_load_indexbuffer(nullptr);
					//gs_vertexbuffer_flush(vbuf);
					const char *techName = "Draw";
					gs_technique_t *tech = gs_effect_get_technique(default_effect, techName);

					size_t passes = gs_technique_begin(tech);
					for (i = 0; i < passes; i++) {
						gs_technique_begin_pass(tech, i);
						gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"), t);
						gs_technique_end_pass(tech);
					}
					//gs_draw(GS_TRISTRIP, 0, 4);
					gs_technique_end(tech);
					/*
					while (gs_effect_loop(default_effect, "Draw")) {
						gs_effect_set_texture(gs_effect_get_param_by_name(default_effect, "image"), t);
						gs_draw(GS_TRISTRIP, 0, 4);
					}
					*/
				}

				gs_texrender_end(_particlerender);

				//gs_texture_t *tex = gs_texrender_get_texture(_texrender);
				gs_texture_t *tex = gs_texrender_get_texture(_texrender);
				_param->setValue<gs_texture_t *>(&tex, sizeof(gs_texture_t *));
			}
			gs_vertexbuffer_destroy(vbuf);
			//gs_vbdata_destroy(m_vertexbufferdata);
			//bfree(m_vertexbufferdata);
		} else {
			if (_param)
				_param->setValue<gs_texture_t *>(&t, sizeof(gs_texture_t *));
		}
	}

	void onPass(ShaderSource *filter, const char *technique, size_t pass, gs_texture_t *texture)
	{
		UNUSED_PARAMETER(filter);
		if (_texType == buffer) {
			std::string tech = technique;
			if (tech == _tech && pass == _pass) {
				//gs_copy_texture();
				double tw = 0;
				double th = 0;
				size_t bytes = 0;
				size_t size = 0;
				if (_tex) {
					tw = gs_texture_get_width(_tex);
					th = gs_texture_get_height(_tex);
				}
				bytes = 4 * 4 * tw * th;
				size = 4 * 4 * gs_texture_get_width(texture) * gs_texture_get_height(texture);

				if (!_data || bytes != size) {
					_data = (uint8_t*)brealloc(_data, size);
					obs_enter_graphics();
					if (_tex)
						gs_texture_destroy(_tex);
					obs_leave_graphics();
					_tex = nullptr;
				}
				if (!_tex)
					_tex = gs_texture_create(gs_texture_get_width(texture), gs_texture_get_height(texture), gs_texture_get_color_format(texture), 1, (const uint8_t **)&_data, 0);

				obs_enter_graphics();
				gs_copy_texture(_tex, texture);
				_param->setValue<gs_texture_t *>(&_tex, sizeof(gs_texture *));
				obs_leave_graphics();
			}
		}
	}

	void onTechniqueEnd(ShaderSource *filter, const char *technique, gs_texture_t *texture)
	{
		UNUSED_PARAMETER(filter);
		if (_texType == buffer) {
			std::string tech = technique;
			if (tech == _tech && _pass == -1) {
				double tw = 0;
				double th = 0;
				size_t bytes = 0;
				size_t size = 0;
				if (_tex) {
					tw = gs_texture_get_width(_tex);
					th = gs_texture_get_height(_tex);
				}
				bytes = 4 * 4 * tw * th;
				size = 4 * 4 * gs_texture_get_width(texture) * gs_texture_get_height(texture);

				if (!_data || bytes != size) {
					_data = (uint8_t*)brealloc(_data, size);
					obs_enter_graphics();
					if (_tex)
						gs_texture_destroy(_tex);
					obs_leave_graphics();
					_tex = nullptr;
				}
				if (!_tex)
					_tex = gs_texture_create(gs_texture_get_width(texture), gs_texture_get_height(texture), gs_texture_get_color_format(texture), 1, (const uint8_t **)&_data, 0);

				obs_enter_graphics();
				gs_copy_texture(_tex, texture);
				_param->setValue<gs_texture_t *>(&_tex, sizeof(gs_texture *));
				obs_leave_graphics();
			}
		}
	}
};

static void sidechain_capture(void *p, obs_source_t *source, const struct audio_data *audio_data, bool muted)
{
	TextureData *data = static_cast<TextureData *>(p);
	UNUSED_PARAMETER(source);
	if (!audio_data->frames)
		return;
	size_t i;
	if (muted) {
		for (i = 0; i < data->getAudioChannels(); i++)
			data->insertAudio(nullptr, audio_data->frames, i);
	} else {
		for (i = 0; i < data->getAudioChannels(); i++)
			data->insertAudio((float *)audio_data->data[i], audio_data->frames, i);
	}
}

class NullData : public ShaderData {
public:
	NullData(ShaderParameter *parent, ShaderSource *filter) : ShaderData(parent, filter)
	{
	};
	~NullData()
	{
	};
	void init(gs_shader_param_type paramType)
	{
		UNUSED_PARAMETER(paramType);
	}
};

std::string ShaderParameter::getName()
{
	return _name;
}

std::string ShaderParameter::getDescription()
{
	return _description;
}

EParam *ShaderParameter::getParameter()
{
	return _param;
}

gs_shader_param_type ShaderParameter::getParameterType()
{
	return _paramType;
}

ShaderParameter::ShaderParameter(gs_eparam_t *param, ShaderSource *filter) : _filter(filter)
{
	struct gs_effect_param_info info;
	gs_effect_get_param_info(param, &info);

	_mutex = new PThreadMutex();
	_name = info.name;
	_description = info.name;
	_param = new EParam(param);

	init(info.type);
}

void ShaderParameter::init(gs_shader_param_type paramType)
{
	_paramType = paramType;
	switch (paramType) {
	case GS_SHADER_PARAM_BOOL:
	case GS_SHADER_PARAM_INT:
	case GS_SHADER_PARAM_INT2:
	case GS_SHADER_PARAM_INT3:
	case GS_SHADER_PARAM_INT4:
	case GS_SHADER_PARAM_FLOAT:
	case GS_SHADER_PARAM_VEC2:
	case GS_SHADER_PARAM_VEC3:
	case GS_SHADER_PARAM_VEC4:
	case GS_SHADER_PARAM_MATRIX4X4:
		_shaderData = new NumericalData(this, _filter);
		break;
	case GS_SHADER_PARAM_TEXTURE:
		_shaderData = new TextureData(this, _filter);
		break;
	case GS_SHADER_PARAM_STRING:
		_shaderData = new StringData(this, _filter);
		break;
	case GS_SHADER_PARAM_UNKNOWN:
		_shaderData = new NullData(this, _filter);
		break;
	}
	if (_shaderData)
		_shaderData->init(paramType);
}

ShaderParameter::~ShaderParameter()
{
	if (_param)
		delete _param;
	_param = nullptr;

	if (_shaderData)
		delete _shaderData;
	_shaderData = nullptr;

	if (_mutex)
		delete _mutex;
	_mutex = nullptr;
}

void ShaderParameter::lock()
{
	if (_mutex)
		_mutex->lock();
}

void ShaderParameter::unlock()
{
	if (_mutex)
		_mutex->unlock();
}

void ShaderParameter::videoTick(ShaderSource *filter, float elapsed_time, float seconds)
{
	if (_shaderData)
		_shaderData->videoTick(filter, elapsed_time, seconds);
}

void ShaderParameter::videoRender(ShaderSource *filter)
{
	if (_shaderData)
		_shaderData->videoRender(filter);
}

void ShaderParameter::update(ShaderSource *filter)
{
	if (_shaderData)
		_shaderData->update(filter);
}

void ShaderParameter::getProperties(ShaderSource *filter, obs_properties_t *props)
{
	if (_shaderData)
		_shaderData->getProperties(filter, props);
}

void ShaderParameter::onPass(ShaderSource *filter, const char *technique, size_t pass, gs_texture_t *texture)
{
	if (_shaderData)
		_shaderData->onPass(filter, technique, pass, texture);
}

void ShaderParameter::onTechniqueEnd(ShaderSource *filter, const char *technique, gs_texture_t *texture)
{
	if (_shaderData)
		_shaderData->onTechniqueEnd(filter, technique, texture);
}

obs_data_t *ShaderSource::getSettings()
{
	return _settings;
}

std::string ShaderSource::getPath()
{
	return _effectPath;
}

void ShaderSource::setPath(std::string path)
{
	_effectPath = path;
}

void ShaderSource::prepReload()
{
	_reloadEffect = true;
}

bool ShaderSource::needsReloading()
{
	return _reloadEffect;
}

std::vector<ShaderParameter *> ShaderSource::parameters()
{
	return paramList;
}

void ShaderSource::clearExpression()
{
	expression.clear();
}

void ShaderSource::appendVariable(te_variable var)
{
	if (!expression.hasVariable(std::string(var.name))) {
		blog(LOG_DEBUG, "appending %s", var.name);
		expression.push_back(var);
		/* Enforce alphabetical order for binary search */
		std::sort(expression.begin(), expression.end(), [](te_variable a, te_variable b) {
			return strcmp(a.name, b.name) < 0;
		});
	} else {
		blog(LOG_WARNING, "%s already appended", var.name);
	}
}

void ShaderSource::appendVariable(std::string &name, double *binding)
{
	te_variable var = { 0 };
	var.address = binding;
	var.name = name.c_str();
	if (!expression.hasVariable(name)) {
		blog(LOG_DEBUG, "appending %s", var.name);
		expression.push_back(var);
		/* Enforce alphabetical order for binary search */
		std::sort(expression.begin(), expression.end(), [](te_variable a, te_variable b) {
			return strcmp(a.name, b.name) < 0;
		});
	} else {
		blog(LOG_WARNING, "%s already appended", var.name);
	}
}

void ShaderSource::compileExpression(std::string expresion)
{
	expression.compile(expresion);
	if (!expressionCompiled())
		blog(LOG_WARNING, "%s", obs_source_get_name(obs_filter_get_parent(context)));
}

bool ShaderSource::expressionCompiled()
{
	return expression;
}

std::string ShaderSource::expressionError()
{
	return expression.errorString();
}

template<class DataType> DataType ShaderSource::evaluateExpression(DataType default_value)
{
	return expression.evaluate(default_value);
}

ShaderSource::ShaderSource(obs_data_t *settings, obs_source_t *source)
{
	context = source;
	_source_type = obs_source_get_type(source);
	_settings = settings;
	_mutex = new PThreadMutex();
#if __linux__ || __FreeBSD__
	dpy = xdisp;
#endif
	prepReload();
	update(this, _settings);
};

ShaderSource::~ShaderSource()
{
	/* Clear previous settings */
	while (!paramList.empty()) {
		ShaderParameter *p = paramList.back();
		paramList.pop_back();
		delete p;
	}

	obs_enter_graphics();
	gs_effect_destroy(effect);
	effect = nullptr;
	gs_texrender_destroy(filterTexrender);
	filterTexrender = nullptr;
	obs_leave_graphics();

	if (_mutex)
		delete _mutex;
};

void ShaderSource::lock()
{
	if (_mutex)
		_mutex->lock();
}

void ShaderSource::unlock()
{
	if (_mutex)
		_mutex->unlock();
}

uint32_t ShaderSource::getWidth()
{
	return totalWidth;
}
uint32_t ShaderSource::getHeight()
{
	return totalHeight;
}

void ShaderSource::updateCache(gs_eparam_t *param)
{
	ShaderParameter *p = new ShaderParameter(param, this);
	if (p) {
		paramList.push_back(p);
		paramMap.insert(std::pair<std::string, ShaderParameter *>(p->getName(), p));
		blog(LOG_INFO, "%s", p->getName().c_str());
	}
}

void ShaderSource::reload()
{
	_reloadEffect = false;
	size_t i;
	char * errors = NULL;

	/* Clear previous settings */
	while (!paramList.empty()) {
		ShaderParameter *p = paramList.back();
		paramList.pop_back();
		delete p;
	}
	for (i = 0; i < resizeExpressions->size(); i++)
		resizeExpressions[i] = "";
	paramMap.clear();
	evaluationList.clear();
	expression.clear();

	prepFunctions(&expression, this);
	/* Enforce alphabetical order for binary search */
	std::sort(expression.begin(), expression.end(), [](te_variable a, te_variable b) {
		return strcmp(a.name, b.name) < 0;
	});

	obs_enter_graphics();
	gs_effect_destroy(effect);
	effect = nullptr;
	obs_leave_graphics();

	_effectPath = obs_data_get_string(_settings, "shader_file_name");
	/* Load default effect text if no file is selected */
	char *effect_string = nullptr;
	if (!_effectPath.empty())
		effect_string = os_quick_read_utf8_file(_effectPath.c_str());
	else
		return;

	obs_enter_graphics();
	effect = gs_effect_create(effect_string, NULL, &errors);
	obs_leave_graphics();

	_effectString = effect_string;
	bfree(effect_string);

	/* Create new parameters */
	size_t effect_count = gs_effect_get_num_params(effect);
	paramList.reserve(effect_count + paramList.size());
	paramMap.reserve(effect_count + paramList.size());
	for (i = 0; i < effect_count; i++) {
		gs_eparam_t *param = gs_effect_get_param_by_idx(effect, i);
		updateCache(param);
	}

	/* Enforce alphabetical order for binary search */
	std::sort(expression.begin(), expression.end(), [](te_variable a, te_variable b) {
		return strcmp(a.name, b.name) < 0;
	});

	if (paramMap.count("image")) {
		ShaderParameter *p = paramMap.at("image");
		image = p->getParameter()->getParam();
	} else {
		image = nullptr;
	}

	if (paramMap.count("image_2")) {
		ShaderParameter *p = paramMap.at("image_2");
		image_2 = p->getParameter()->getParam();
	} else {
		image_2 = nullptr;
	}
}

void *ShaderSource::create(obs_data_t *settings, obs_source_t *source)
{
	ShaderSource *filter = new ShaderSource(settings, source);
	return filter;
}

void ShaderSource::destroy(void *data)
{
	delete static_cast<ShaderSource *>(data);
}

const char *ShaderSource::getName(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ShaderSource");
}

void ShaderSource::videoTick(void *data, float seconds)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	filter->elapsedTimeBinding.d += seconds;
	filter->elapsedTime += seconds;

	getMouseCursor(filter);
	getScreenSizes(filter);


	size_t i;
	for (i = 0; i < filter->paramList.size(); i++) {
		if (filter->paramList[i])
			filter->paramList[i]->videoTick(filter, filter->elapsedTime, seconds);
	}

	int *resize[4] = { &filter->resizeLeft, &filter->resizeRight, &filter->resizeTop, &filter->resizeBottom };
	for (i = 0; i < 4; i++) {
		if (filter->resizeExpressions[i].empty())
			continue;
		filter->compileExpression(filter->resizeExpressions[i]);
		if (filter->expressionCompiled())
			*resize[i] = filter->evaluateExpression<int>(0);
	}

	obs_source_t *target = obs_filter_get_target(filter->context);
	/* Determine offsets from expansion values. */
	int baseWidth = obs_source_get_base_width(target);
	int baseHeight = obs_source_get_base_height(target);

	filter->totalWidth = filter->resizeLeft + baseWidth + filter->resizeRight;
	filter->totalHeight = filter->resizeTop + baseHeight + filter->resizeBottom;

	filter->uvScale.x = (float)filter->totalWidth / baseWidth;
	filter->uvScale.y = (float)filter->totalHeight / baseHeight;
	filter->uvOffset.x = (float)(-filter->resizeLeft) / baseWidth;
	filter->uvOffset.y = (float)(-filter->resizeTop) / baseHeight;
	filter->uvPixelInterval.x = 1.0f / baseWidth;
	filter->uvPixelInterval.y = 1.0f / baseHeight;

	filter->uvScaleBinding = filter->uvScale;
	filter->uvOffsetBinding = filter->uvOffset;
}

void ShaderSource::videoTickSource(void *data, float seconds)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	filter->elapsedTimeBinding.d += seconds;
	filter->elapsedTime += seconds;

	getMouseCursor(filter);
	getScreenSizes(filter);

	size_t i;
	for (i = 0; i < filter->paramList.size(); i++) {
		if (filter->paramList[i])
			filter->paramList[i]->videoTick(filter, filter->elapsedTime, seconds);
	}

	int *resize[4] = { &filter->resizeLeft, &filter->resizeRight, &filter->resizeTop, &filter->resizeBottom };
	for (i = 0; i < 4; i++) {
		if (filter->resizeExpressions[i].empty())
			continue;
		filter->compileExpression(filter->resizeExpressions[i]);
		if (filter->expressionCompiled())
			*resize[i] = filter->evaluateExpression<int>(0);
	}

	/* Determine offsets from expansion values. */
	int baseWidth = filter->baseWidth;
	int baseHeight = filter->baseHeight;

	filter->totalWidth = filter->resizeLeft + baseWidth + filter->resizeRight;
	filter->totalHeight = filter->resizeTop + baseHeight + filter->resizeBottom;

	filter->uvScale.x = (float)filter->totalWidth / baseWidth;
	filter->uvScale.y = (float)filter->totalHeight / baseHeight;
	filter->uvOffset.x = (float)(-filter->resizeLeft) / baseWidth;
	filter->uvOffset.y = (float)(-filter->resizeTop) / baseHeight;
	filter->uvPixelInterval.x = 1.0f / baseWidth;
	filter->uvPixelInterval.y = 1.0f / baseHeight;

	filter->uvScaleBinding = filter->uvScale;
	filter->uvOffsetBinding = filter->uvOffset;
}

static inline void renderSprite(ShaderSource *filter, gs_effect_t *effect, gs_texture_t *texture, const char *techName, uint32_t &cx, uint32_t &cy)
{
	size_t i, j;
	gs_technique_t *tech = gs_effect_get_technique(effect, techName);
	size_t passes = gs_technique_begin(tech);
	for (i = 0; i < passes; i++) {
		gs_technique_begin_pass(tech, i);
		gs_draw_sprite(texture, 0, cx, cy);
		gs_technique_end_pass(tech);
		/*Handle Buffers*/
		for (j = 0; j < filter->paramList.size(); j++)
			filter->paramList[j]->onPass(filter, techName, i, texture);
	}
	gs_technique_end(tech);
	for (j = 0; j < filter->paramList.size(); j++)
		filter->paramList[j]->onTechniqueEnd(filter, techName, texture);
}

void ShaderSource::videoRender(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	size_t        passes, i, j;

	if (filter->effect != nullptr) {
		obs_source_t *target, *parent, *source;
		gs_texture_t *texture;
		uint32_t      parentFlags;

		source = filter->context;
		target = obs_filter_get_target(filter->context);
		parent = obs_filter_get_parent(filter->context);

		if (!target) {
			blog(LOG_INFO, "filter '%s' being processed with no target!",
				obs_source_get_name(filter->context));
			return;
		}
		if (!parent) {
			blog(LOG_INFO, "filter '%s' being processed with no parent!",
				obs_source_get_name(filter->context));
			return;
		}

		uint32_t cx = filter->totalWidth;
		uint32_t cy = filter->totalHeight;

		if (!cx || !cy) {
			obs_source_skip_video_filter(filter->context);
			return;
		}

		for (i = 0; i < filter->paramList.size(); i++) {
			if (filter->paramList[i])
				filter->paramList[i]->videoRender(filter);
		}

		if (!filter->filterTexrender)
			filter->filterTexrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

		const char *id = obs_source_get_id(parent);
		parentFlags = obs_get_source_output_flags(id);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		gs_texrender_reset(filter->filterTexrender);
		if (gs_texrender_begin(filter->filterTexrender, cx, cy)) {
			bool        customDraw = (parentFlags & OBS_SOURCE_CUSTOM_DRAW) != 0;
			bool        async = (parentFlags & OBS_SOURCE_ASYNC) != 0;
			struct vec4 clearColor;

			vec4_zero(&clearColor);
			gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

			if (target == parent && !customDraw && !async)
				obs_source_default_render(target);
			else
				obs_source_video_render(target);

			gs_texrender_end(filter->filterTexrender);
		}

		gs_blend_state_pop();

		enum obs_allow_direct_render allowBypass = OBS_NO_DIRECT_RENDERING;
		bool canBypass = (target == parent) && (allowBypass == OBS_ALLOW_DIRECT_RENDERING) &&
			((parentFlags & OBS_SOURCE_CUSTOM_DRAW) == 0) &&
			((parentFlags & OBS_SOURCE_ASYNC) == 0);

		const char *techName = "Draw";

		if (canBypass) {
			gs_technique_t *tech = gs_effect_get_technique(filter->effect, techName);
			texture = gs_texrender_get_texture(filter->filterTexrender);

			passes = gs_technique_begin(tech);
			for (i = 0; i < passes; i++) {
				gs_technique_begin_pass(tech, i);
				obs_source_video_render(target);
				gs_technique_end_pass(tech);
				/*Handle Buffers*/
				for (j = 0; j < filter->paramList.size(); j++)
					filter->paramList[j]->onPass(filter, techName, i, texture);
			}
			gs_technique_end(tech);
			for (j = 0; j < filter->paramList.size(); j++)
				filter->paramList[j]->onTechniqueEnd(filter, techName, texture);
		} else {
			texture = gs_texrender_get_texture(filter->filterTexrender);
			if (texture) {
				gs_technique_t *tech = gs_effect_get_technique(filter->effect, techName);
				if (filter->image)
					gs_effect_set_texture(filter->image, texture);

				renderSprite(filter, filter->effect, texture, techName, cx, cy);
			}
		}
	} else {
		obs_source_skip_video_filter(filter->context);
	}
}

void ShaderSource::videoRenderSource(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	size_t        passes, i, j;

	obs_source_t *source;
	gs_texture_t *texture;
	uint32_t      parentFlags;

	source = filter->context;

	if (!source) {
		blog(LOG_INFO, "no source?");
		return;
	}

	uint32_t cx = obs_source_get_base_width(source);
	uint32_t cy = obs_source_get_base_height(source);

	if (!cx || !cy)
		return;

	if (filter->effect != nullptr) {
		for (i = 0; i < filter->paramList.size(); i++) {
			if (filter->paramList[i])
				filter->paramList[i]->videoRender(filter);
		}

		if (!filter->filterTexrender)
			filter->filterTexrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

		const char *id = obs_source_get_id(source);
		parentFlags = obs_get_source_output_flags(id);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		gs_texrender_reset(filter->filterTexrender);
		if (gs_texrender_begin(filter->filterTexrender, cx, cy)) {
			struct vec4 clearColor;

			vec4_zero(&clearColor);
			gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

			gs_texrender_end(filter->filterTexrender);
		}

		gs_blend_state_pop();

		texture = gs_texrender_get_texture(filter->filterTexrender);
		if (texture) {
			const char *techName = "Draw";
			if (filter->image)
				gs_effect_set_texture(filter->image, texture);
			renderSprite(filter, filter->effect, texture, techName, filter->totalWidth, filter->totalHeight);
		}
	} else {
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		gs_texrender_reset(filter->filterTexrender);
		if (gs_texrender_begin(filter->filterTexrender, cx, cy)) {
			struct vec4 clearColor;

			vec4_zero(&clearColor);
			gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

			gs_texrender_end(filter->filterTexrender);
		}

		gs_blend_state_pop();
		texture = gs_texrender_get_texture(filter->filterTexrender);
		if (texture) {
			const char *techName = "Draw";
			gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *img = gs_effect_get_param_by_name(effect, "image");
			if (img)
				gs_effect_set_texture(img, texture);
			renderSprite(filter, effect, texture, techName, filter->totalWidth, filter->totalHeight);
		}
	}
}

void ShaderSource::videoTickTransition(void *data, float seconds)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(seconds);
}

static void renderTransition(void *data, gs_texture_t *a, gs_texture_t *b,
	float t, uint32_t cx, uint32_t cy)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	size_t i, j, passes;
	uint32_t      parentFlags;
	gs_texture_t *texture;
	obs_source_t *source = filter->context;

	uint64_t ts = os_gettime_ns();

	filter->transitionPercentage = t;
	float seconds = (ts / 1000000000.0);
	filter->elapsedTimeBinding.d = seconds;
	filter->elapsedTime = seconds;
	filter->transitionSeconds = ((filter->startTimestamp - ts) / 1000000000.0);

	getMouseCursor(filter);
	getScreenSizes(filter);

	for (i = 0; i < filter->paramList.size(); i++) {
		if (filter->paramList[i])
			filter->paramList[i]->videoTick(filter, filter->elapsedTime, seconds);
	}

	int baseWidth = cx;
	int baseHeight = cy;

	filter->totalWidth = baseWidth;
	filter->totalHeight = baseHeight;

	filter->uvScale.x = (float)filter->totalWidth / baseWidth;
	filter->uvScale.y = (float)filter->totalHeight / baseHeight;
	filter->uvOffset.x = (float)(-filter->resizeLeft) / baseWidth;
	filter->uvOffset.y = (float)(-filter->resizeTop) / baseHeight;
	filter->uvPixelInterval.x = 1.0f / baseWidth;
	filter->uvPixelInterval.y = 1.0f / baseHeight;

	filter->uvScaleBinding = filter->uvScale;
	filter->uvOffsetBinding = filter->uvOffset;

	if (filter->effect != nullptr) {
		for (i = 0; i < filter->paramList.size(); i++) {
			if (filter->paramList[i])
				filter->paramList[i]->videoRender(filter);
		}

		if (!filter->filterTexrender)
			filter->filterTexrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);


		const char *id = obs_source_get_id(source);
		parentFlags = obs_get_source_output_flags(id);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

		gs_texrender_reset(filter->filterTexrender);
		if (gs_texrender_begin(filter->filterTexrender, cx, cy)) {
			struct vec4 clearColor;

			vec4_zero(&clearColor);
			gs_clear(GS_CLEAR_COLOR, &clearColor, 0.0f, 0);
			gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

			gs_texrender_end(filter->filterTexrender);
		}

		gs_blend_state_pop();

		texture = gs_texrender_get_texture(filter->filterTexrender);

		if (a || b) {
			const char *techName = "Draw";
			if (filter->image)
				gs_effect_set_texture(filter->image, a);
			if (filter->image_2)
				gs_effect_set_texture(filter->image_2, b);
			renderSprite(filter, filter->effect, texture, techName, cx, cy);
		}
	} else {
		/* Cut Effect */
		texture = b;

		if (texture) {
			const char *techName = "Draw";

			gs_effect_t *   effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
			if (!filter->image)
				filter->image = gs_effect_get_param_by_name(effect, "image");

			gs_effect_set_texture(filter->image, texture);
			renderSprite(filter, effect, texture, techName, cx, cy);
		}
	}
}

void ShaderSource::videoRenderTransition(void *data, gs_effect_t *effect)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	obs_transition_video_render(filter->context, renderTransition);
	UNUSED_PARAMETER(effect);
}

void ShaderSource::transitionStart(void *data)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	filter->startTimestamp = os_gettime_ns();
}

void ShaderSource::transitionStop(void *data)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	filter->stopTimestamp = os_gettime_ns();
}

static float mix_a(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return 1.0f - t;
}

static float mix_b(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return t;
}

bool ShaderSource::audioRenderTransition(void *data, uint64_t *ts_out,
	struct obs_source_audio_mix *audio, uint32_t mixers,
	size_t channels, size_t sample_rate)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	return obs_transition_audio_render(filter->context, ts_out, audio, mixers,
		channels, sample_rate, mix_a, mix_b);
}

void ShaderSource::update(void *data, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	if (filter->needsReloading()) {
		filter->reload();
		obs_source_update_properties(filter->context);
	}
	size_t i;
	for (i = 0; i < filter->paramList.size(); i++) {
		if (filter->paramList[i])
			filter->paramList[i]->update(filter);
	}
	filter->baseHeight = (int)obs_data_get_int(settings, "size.height");
	filter->baseWidth = (int)obs_data_get_int(settings, "size.width");
}

obs_properties_t *ShaderSource::getProperties(void *data)
{
	ShaderSource *    filter = static_cast<ShaderSource *>(data);
	size_t            i;
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, filter, NULL);

	std::string shaderPath = obs_get_module_data_path(obs_current_module());
	shaderPath += "/shaders";

	obs_properties_add_button(
		props, "reload_effect", obs_module_text("Reload"), shader_filter_reload_effect_clicked);

	obs_property_t *file_name = obs_properties_add_path(
		props, "shader_file_name", obs_module_text("File"), OBS_PATH_FILE, NULL, shaderPath.c_str());

	obs_property_set_modified_callback(file_name, shader_filter_file_name_changed);

	for (i = 0; i < filter->paramList.size(); i++) {
		if (filter->paramList[i])
			filter->paramList[i]->getProperties(filter, props);
	}
	return props;
}

obs_properties_t *ShaderSource::getPropertiesSource(void *data)
{
	ShaderSource *    filter = static_cast<ShaderSource *>(data);
	size_t            i;
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, filter, NULL);

	std::string shaderPath = obs_get_module_data_path(obs_current_module());
	shaderPath += "/shaders";

	obs_properties_add_button(
		props, "reload_effect", obs_module_text("Reload"), shader_filter_reload_effect_clicked);

	obs_property_t *file_name = obs_properties_add_path(
		props, "shader_file_name", obs_module_text("File"), OBS_PATH_FILE, NULL, shaderPath.c_str());

	obs_property_set_modified_callback(file_name, shader_filter_file_name_changed);

	obs_properties_add_int(props, "size.width", obs_module_text("Width"), 0, 4096, 1);

	obs_properties_add_int(props, "size.height", obs_module_text("Height"), 0, 4096, 1);

	for (i = 0; i < filter->paramList.size(); i++) {
		if (filter->paramList[i])
			filter->paramList[i]->getProperties(filter, props);
	}
	return props;
}

uint32_t ShaderSource::getWidth(void *data)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	return filter->getWidth();
}

uint32_t ShaderSource::getHeight(void *data)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	return filter->getHeight();
}

static void getMouseCursor(void *data)
{
#ifdef _WIN32
	ShaderSource *filter = static_cast<ShaderSource *>(data);

	CURSORINFO ci = { 0 };
	HICON icon;

	ci.cbSize = sizeof(ci);

	if (!GetCursorInfo(&ci)) {
		filter->_screenMouseVisible = false;
		return;
	}

	filter->_screenMousePosX = ci.ptScreenPos.x;
	filter->_screenMousePosY = ci.ptScreenPos.y;
	filter->_screenMouseVisible = false;
#elif __linux__ || __FreeBSD__
	XFixesCursorImage *xc = XFixesGetCursorImage(filter->dpy);
	if (!xc)
		return;

	filter->_screenMousePosX = (double)xc->x;
	filter->_screenMousePosY = (double)xc->y;
	filter->_screenMouseVisible = true;
	UNUSED_PARAMETER(data);
#endif
}

static void getScreenSizes(void *data)
{
#ifdef _WIN32
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	filter->_wholeScreenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
	filter->_wholeScreenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);

	filter->_primaryScreenWidth = GetSystemMetrics(SM_CXSCREEN);
	filter->_primaryScreenHeight = GetSystemMetrics(SM_CYSCREEN);
	//int i = GetSystemMetrics(SM_CMONITORS);
#elif __linux__ || __FreeBSD__
	UNUSED_PARAMETER(data);
#endif
}

void ShaderSource::mouseClick(
	void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up, uint32_t click_count)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	filter->_mouseType = type;
	filter->_mouseUp = mouse_up;
	filter->_clickCount = click_count;
	filter->_mouseX = event->x;
	filter->_mouseY = event->y;
	filter->_mouseClickX = event->x;
	filter->_mouseClickY = event->y;
}

void ShaderSource::mouseMove(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	filter->_mouseX = event->x;
	filter->_mouseY = event->y;
	filter->_clickCount = 0;
	filter->_mouseLeave = mouse_leave;
}

void ShaderSource::mouseWheel(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	filter->_mouseX = event->x;
	filter->_mouseY = event->y;
	filter->_mouseWheelDeltaX = x_delta;
	filter->_mouseWheelDeltaY = y_delta;
	filter->_mouseWheelX += x_delta;
	filter->_mouseWheelY += y_delta;
}

void ShaderSource::focus(void *data, bool focus)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	filter->_focus = focus ? 1.0 : 0.0;
}

void ShaderSource::keyClick(void *data, const struct obs_key_event *event, bool key_up)
{
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	filter->_keyModifiers = event->modifiers;
	filter->_nativeKeyModifiers = event->native_modifiers;
	if (event->text)
		filter->_key = (double)event->text[0];
	filter->_keyUp = key_up;
}

void ShaderSource::getDefaults(obs_data_t *settings)
{
	struct obs_video_info ovi;
	obs_get_video_info(&ovi);
	obs_data_set_default_int(settings, "size.height", ovi.base_height);
	obs_data_set_default_int(settings, "size.width", ovi.base_width);
}

static bool shader_filter_reload_effect_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(property);
	UNUSED_PARAMETER(props);
	ShaderSource *filter = static_cast<ShaderSource *>(data);
	filter->prepReload();
	obs_source_update(filter->context, NULL);

	return true;
}

static bool shader_filter_file_name_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	ShaderSource *filter = static_cast<ShaderSource *>(obs_properties_get_param(props));
	std::string   path = obs_data_get_string(settings, obs_property_name(p));

	if (filter->getPath() != path) {
		filter->prepReload();
		filter->setPath(path);
		obs_source_update(filter->context, NULL);
	}

	return true;
}

bool obs_module_load(void)
{
	struct obs_source_info shader_filter = { 0 };
	shader_filter.id = "obs_shader_filter";
	shader_filter.type = OBS_SOURCE_TYPE_FILTER;
	shader_filter.output_flags = OBS_SOURCE_VIDEO;
	shader_filter.get_name = ShaderSource::getName;
	shader_filter.create = ShaderSource::create;
	shader_filter.destroy = ShaderSource::destroy;
	shader_filter.update = ShaderSource::update;
	shader_filter.video_tick = ShaderSource::videoTick;
	shader_filter.video_render = ShaderSource::videoRender;
	shader_filter.get_defaults = ShaderSource::getDefaults;
	shader_filter.get_width = ShaderSource::getWidth;
	shader_filter.get_height = ShaderSource::getHeight;
	shader_filter.get_properties = ShaderSource::getProperties;

	obs_register_source(&shader_filter);

	struct obs_source_info shader_source = { 0 };
	shader_source.id = "obs_shader_source";
	shader_source.type = OBS_SOURCE_TYPE_INPUT;
	shader_source.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_INTERACTION;
	shader_source.get_name = ShaderSource::getName;
	shader_source.create = ShaderSource::create;
	shader_source.destroy = ShaderSource::destroy;
	shader_source.update = ShaderSource::update;
	shader_source.video_tick = ShaderSource::videoTickSource;
	shader_source.video_render = ShaderSource::videoRenderSource;
	shader_source.get_defaults = ShaderSource::getDefaults;
	shader_source.get_width = ShaderSource::getWidth;
	shader_source.get_height = ShaderSource::getHeight;
	shader_source.get_properties = ShaderSource::getPropertiesSource;
	/* Interaction Callbacks */
	shader_source.mouse_click = ShaderSource::mouseClick;
	shader_source.mouse_move = ShaderSource::mouseMove;
	shader_source.mouse_wheel = ShaderSource::mouseWheel;
	shader_source.focus = ShaderSource::focus;
	shader_source.key_click = ShaderSource::keyClick;

	obs_register_source(&shader_source);

	struct obs_source_info shader_transition = { 0 };
	shader_transition.id = "obs_shader_transition";
	shader_transition.type = OBS_SOURCE_TYPE_TRANSITION;
	shader_transition.output_flags = OBS_SOURCE_VIDEO;
	shader_transition.get_name = ShaderSource::getName;
	shader_transition.create = ShaderSource::create;
	shader_transition.destroy = ShaderSource::destroy;
	shader_transition.update = ShaderSource::update;
	//shader_transition.video_tick = ShaderSource::videoTickSource;
	shader_transition.video_render = ShaderSource::videoRenderTransition;
	shader_transition.audio_render = ShaderSource::audioRenderTransition;
	shader_transition.get_properties = ShaderSource::getProperties;
	shader_transition.get_defaults = ShaderSource::getDefaults;
	shader_transition.transition_start = ShaderSource::transitionStart;
	shader_transition.transition_stop = ShaderSource::transitionStop;

	obs_register_source(&shader_transition);

	struct obs_audio_info aoi;
	obs_get_audio_info(&aoi);
	sample_rate = (double)aoi.samples_per_sec;
	output_channels = (double)get_audio_channels(aoi.speakers);

	return true;
}

void obs_module_unload(void)
{
}
