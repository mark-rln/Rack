#include <app/ModuleBrowser.hpp>
#include <widget/OpaqueWidget.hpp>
#include <widget/TransparentWidget.hpp>
#include <widget/ZoomWidget.hpp>
#include <ui/ScrollWidget.hpp>
#include <ui/SequentialLayout.hpp>
#include <ui/MarginLayout.hpp>
#include <ui/Label.hpp>
#include <ui/TextField.hpp>
#include <ui/MenuOverlay.hpp>
#include <ui/List.hpp>
#include <ui/MenuItem.hpp>
#include <ui/Button.hpp>
#include <ui/RadioButton.hpp>
#include <ui/ChoiceButton.hpp>
#include <ui/Tooltip.hpp>
#include <app/ModuleWidget.hpp>
#include <app/Scene.hpp>
#include <plugin.hpp>
#include <app.hpp>
#include <plugin/Model.hpp>
#include <string.hpp>
#include <history.hpp>
#include <settings.hpp>

#include <set>
#include <algorithm>


namespace rack {
namespace app {


static float modelScore(plugin::Model *model, const std::string &search) {
	if (search.empty())
		return 1.f;
	std::string s;
	s += model->plugin->brand;
	s += " ";
	s += model->name;
	s += " ";
	s += model->slug;
	for (const std::string &tag : model->tags) {
		s += " ";
		s += tag;
	}
	float score = string::fuzzyScore(string::lowercase(s), string::lowercase(search));
	return score;
}

static bool isModelVisible(plugin::Model *model, const std::string &search, const std::string &brand, const std::string &tag) {
	// Filter search query
	if (search != "") {
		float score = modelScore(model, search);
		if (score <= 0.f)
			return false;
	}

	// Filter brand
	if (brand != "") {
		if (model->plugin->brand != brand)
			return false;
	}

	// Filter tag
	if (tag != "") {
		bool found = false;
		for (const std::string &modelTag : model->tags) {
			if (modelTag == tag) {
				found = true;
				break;
			}
		}
		if (!found)
			return false;
	}

	return true;
}

static void stepFavoriteScore(const std::string &plugin, const std::string &model) {
	// Decay all scores
	const float decayLambda = 0.1;
	for (auto &it : settings::favoriteScores) {
		it.second *= 1 - decayLambda;
	}
	// Increment favorite score by 1
	settings::favoriteScores[std::make_tuple(plugin, model)] += 1;
}


template <typename K, typename V>
V get_default(const std::map<K, V> &m, const K &key, const V &def) {
	auto it = m.find(key);
	if (it == m.end())
		return def;
	return it->second;
}


struct BrowserOverlay : widget::OpaqueWidget {
	void step() override {
		box = parent->box.zeroPos();
		// Only step if visible, since there are potentially thousands of descendants that don't need to be stepped.
		if (visible)
			OpaqueWidget::step();
	}

	void onButton(const event::Button &e) override {
		OpaqueWidget::onButton(e);
		if (e.getTarget() != this)
			return;

		if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
			hide();
			e.consume(this);
		}
	}
};


static const float MODEL_BOX_ZOOM = 0.5f;


struct ModelBox : widget::OpaqueWidget {
	plugin::Model *model;
	widget::Widget *previewWidget;
	ui::Tooltip *tooltip = NULL;
	/** Lazily created */
	widget::FramebufferWidget *previewFb = NULL;
	/** Number of frames since draw() has been called */
	int visibleFrames = 0;

	ModelBox() {
		// Approximate size as 10HP before we know the actual size.
		// We need a nonzero size, otherwise the parent widget will consider it not in the draw bounds, so its preview will not be lazily created.
		box.size.x = 10 * RACK_GRID_WIDTH * MODEL_BOX_ZOOM;
		box.size.y = RACK_GRID_HEIGHT * MODEL_BOX_ZOOM;
		box.size = box.size.ceil();
	}

	void setModel(plugin::Model *model) {
		this->model = model;

		previewWidget = new widget::TransparentWidget;
		previewWidget->box.size.y = std::ceil(RACK_GRID_HEIGHT * MODEL_BOX_ZOOM);
		addChild(previewWidget);
	}

	void createPreview() {
		previewFb = new widget::FramebufferWidget;
		if (math::isNear(APP->window->pixelRatio, 1.0)) {
			// Small details draw poorly at low DPI, so oversample when drawing to the framebuffer
			previewFb->oversample = 2.0;
		}
		previewWidget->addChild(previewFb);

		widget::ZoomWidget *zoomWidget = new widget::ZoomWidget;
		zoomWidget->setZoom(MODEL_BOX_ZOOM);
		previewFb->addChild(zoomWidget);

		ModuleWidget *moduleWidget = model->createModuleWidgetNull();
		zoomWidget->addChild(moduleWidget);

		zoomWidget->box.size.x = moduleWidget->box.size.x * MODEL_BOX_ZOOM;
		zoomWidget->box.size.y = RACK_GRID_HEIGHT * MODEL_BOX_ZOOM;
		previewWidget->box.size.x = std::ceil(zoomWidget->box.size.x);

		box.size.x = previewWidget->box.size.x;
	}

	void deletePreview() {
		assert(previewFb);
		previewWidget->removeChild(previewFb);
		delete previewFb;
		previewFb = NULL;
	}

	void step() override {
		if (previewFb && ++visibleFrames >= 60) {
			deletePreview();
		}
		OpaqueWidget::step();
	}

	void draw(const DrawArgs &args) override {
		visibleFrames = 0;

		// Lazily create preview when drawn
		if (!previewFb) {
			createPreview();
		}

		// Draw shadow
		nvgBeginPath(args.vg);
		float r = 10; // Blur radius
		float c = 10; // Corner radius
		nvgRect(args.vg, -r, -r, box.size.x + 2*r, box.size.y + 2*r);
		NVGcolor shadowColor = nvgRGBAf(0, 0, 0, 0.5);
		NVGcolor transparentColor = nvgRGBAf(0, 0, 0, 0);
		nvgFillPaint(args.vg, nvgBoxGradient(args.vg, 0, 0, box.size.x, box.size.y, c, r, shadowColor, transparentColor));
		nvgFill(args.vg);

		OpaqueWidget::draw(args);
	}

	void setTooltip(ui::Tooltip *tooltip) {
		if (this->tooltip) {
			this->tooltip->parent->removeChild(this->tooltip);
			delete this->tooltip;
			this->tooltip = NULL;
		}

		if (tooltip) {
			APP->scene->addChild(tooltip);
			this->tooltip = tooltip;
		}
	}

	void onButton(const event::Button &e) override;

	void onEnter(const event::Enter &e) override {
		ui::Tooltip *tooltip = new ui::Tooltip;
		tooltip->text = model->plugin->brand;
		tooltip->text += " " + model->name;
		if (model->description != "")
			tooltip->text += "\n" + model->description;
		setTooltip(tooltip);
	}

	void onLeave(const event::Leave &e) override {
		setTooltip(NULL);
	}
};


struct BrandItem : ui::MenuItem {
	void onAction(const event::Action &e) override;
	void step() override;
};


struct TagItem : ui::MenuItem {
	void onAction(const event::Action &e) override;
	void step() override;
};


struct BrowserSearchField : ui::TextField {
	void step() override {
		// Steal focus when step is called
		APP->event->setSelected(this);
		TextField::step();
	}

	void onSelectKey(const event::SelectKey &e) override {
		if (e.action == GLFW_PRESS) {
			if (e.key == GLFW_KEY_ESCAPE) {
				if (text != "") {
					setText("");
				}
				else {
					BrowserOverlay *overlay = getAncestorOfType<BrowserOverlay>();
					overlay->hide();
				}
				e.consume(this);
			}
		}

		if (!e.getTarget())
			ui::TextField::onSelectKey(e);
	}

	void onChange(const event::Change &e) override;

	void onHide(const event::Hide &e) override {
		APP->event->setSelected(NULL);
		ui::TextField::onHide(e);
	}

	void onShow(const event::Show &e) override {
		selectAll();
		TextField::onShow(e);
	}
};


struct ClearButton : ui::Button {
	void onAction(const event::Action &e) override;
};


struct BrowserSidebar : widget::Widget {
	BrowserSearchField *searchField;
	ClearButton *clearButton;
	ui::Label *brandLabel;
	ui::List *brandList;
	ui::ScrollWidget *brandScroll;
	ui::Label *tagLabel;
	ui::List *tagList;
	ui::ScrollWidget *tagScroll;

	BrowserSidebar() {
		searchField = new BrowserSearchField;
		addChild(searchField);

		clearButton = new ClearButton;
		clearButton->text = "Reset filters";
		addChild(clearButton);

		brandLabel = new ui::Label;
		// brandLabel->fontSize = 16;
		brandLabel->color = nvgRGB(0x80, 0x80, 0x80);
		brandLabel->text = "Brands";
		addChild(brandLabel);

		// Plugin list
		brandScroll = new ui::ScrollWidget;
		addChild(brandScroll);

		brandList = new ui::List;
		brandScroll->container->addChild(brandList);

		std::set<std::string, string::CaseInsensitiveCompare> brands;
		for (plugin::Plugin *plugin : plugin::plugins) {
			brands.insert(plugin->brand);
		}

		for (const std::string &brand : brands) {
			BrandItem *item = new BrandItem;
			item->text = brand;
			brandList->addChild(item);
		}

		tagLabel = new ui::Label;
		// tagLabel->fontSize = 16;
		tagLabel->color = nvgRGB(0x80, 0x80, 0x80);
		tagLabel->text = "Tags";
		addChild(tagLabel);

		// Tag list
		tagScroll = new ui::ScrollWidget;
		addChild(tagScroll);

		tagList = new ui::List;
		tagScroll->container->addChild(tagList);

		for (const std::string &tag : plugin::allowedTags) {
			TagItem *item = new TagItem;
			item->text = tag;
			tagList->addChild(item);
		}
	}

	void step() override {
		searchField->box.size.x = box.size.x;
		clearButton->box.pos = searchField->box.getBottomLeft();
		clearButton->box.size.x = box.size.x;

		float listHeight = (box.size.y - clearButton->box.getBottom()) / 2;
		listHeight = std::floor(listHeight);

		brandLabel->box.pos = clearButton->box.getBottomLeft();
		brandLabel->box.size.x = box.size.x;
		brandScroll->box.pos = brandLabel->box.getBottomLeft();
		brandScroll->box.size.y = listHeight - brandLabel->box.size.y;
		brandScroll->box.size.x = box.size.x;
		brandList->box.size.x = brandScroll->box.size.x;

		tagLabel->box.pos = brandScroll->box.getBottomLeft();
		tagLabel->box.size.x = box.size.x;
		tagScroll->box.pos = tagLabel->box.getBottomLeft();
		tagScroll->box.size.y = listHeight - tagLabel->box.size.y;
		tagScroll->box.size.x = box.size.x;
		tagList->box.size.x = tagScroll->box.size.x;

		Widget::step();
	}
};


struct ModuleBrowser : widget::OpaqueWidget {
	BrowserSidebar *sidebar;
	ui::ScrollWidget *modelScroll;
	ui::Label *modelLabel;
	ui::MarginLayout *modelMargin;
	ui::SequentialLayout *modelContainer;

	std::string search;
	std::string brand;
	std::string tag;

	ModuleBrowser() {
		sidebar = new BrowserSidebar;
		sidebar->box.size.x = 200;
		addChild(sidebar);

		modelScroll = new ui::ScrollWidget;
		addChild(modelScroll);

		modelLabel = new ui::Label;
		// modelLabel->fontSize = 16;
		modelLabel->box.size.x = 200;
		modelLabel->box.pos = math::Vec(10, 10);
		modelScroll->container->addChild(modelLabel);

		modelMargin = new ui::MarginLayout;
		modelMargin->box.pos = modelLabel->box.getBottomLeft();
		modelMargin->margin = math::Vec(10, 10);
		modelScroll->container->addChild(modelMargin);

		modelContainer = new ui::SequentialLayout;
		modelContainer->spacing = math::Vec(10, 10);
		modelMargin->addChild(modelContainer);

		// Add ModelBoxes for each Model
		for (plugin::Plugin *plugin : plugin::plugins) {
			for (plugin::Model *model : plugin->models) {
				ModelBox *moduleBox = new ModelBox;
				moduleBox->setModel(model);
				modelContainer->addChild(moduleBox);
			}
		}

		refresh();
	}

	void step() override {
		box = parent->box.zeroPos().grow(math::Vec(-70, -70));

		sidebar->box.size.y = box.size.y;

		modelScroll->box.pos.x = sidebar->box.size.x;
		modelScroll->box.size.x = box.size.x - sidebar->box.size.x;
		modelScroll->box.size.y = box.size.y;
		modelMargin->box.size.x = modelScroll->box.size.x;
		modelMargin->box.size.y = modelContainer->getChildrenBoundingBox().size.y + 2 * modelMargin->margin.y;

		OpaqueWidget::step();
	}

	void draw(const DrawArgs &args) override {
		bndMenuBackground(args.vg, 0.0, 0.0, box.size.x, box.size.y, 0);
		Widget::draw(args);
	}

	void refresh() {
		// Reset scroll position
		modelScroll->offset = math::Vec();

		// Filter ModelBoxes
		for (Widget *w : modelContainer->children) {
			ModelBox *m = dynamic_cast<ModelBox*>(w);
			assert(m);
			m->visible = isModelVisible(m->model, search, brand, tag);
		}

		// Sort ModelBoxes
		// Sort by favorite score and then name
		modelContainer->children.sort([&](Widget *w1, Widget *w2) {
			ModelBox *m1 = dynamic_cast<ModelBox*>(w1);
			ModelBox *m2 = dynamic_cast<ModelBox*>(w2);
			// Sort by favorite score if either is available
			float score1 = get_default(settings::favoriteScores, std::make_tuple(m1->model->plugin->slug, m1->model->slug), 0.f);
			float score2 = get_default(settings::favoriteScores, std::make_tuple(m2->model->plugin->slug, m2->model->slug), 0.f);
			if (score1 != score2)
				return score1 > score2;
			// Sort by plugin name
			if (m1->model->plugin->name != m2->model->plugin->name)
				return m1->model->plugin->name < m2->model->plugin->name;
			// Sort by module name
			return m1->model->name < m2->model->name;
		});

		if (search.empty()) {
			// We've already sorted above
		}
		else {
			std::map<Widget*, float> scores;
			// Compute scores
			for (Widget *w : modelContainer->children) {
				ModelBox *m = dynamic_cast<ModelBox*>(w);
				assert(m);
				if (!m->visible)
					continue;
				scores[m] = modelScore(m->model, search);
			}
			// // Sort by score
			// modelContainer->children.sort([&](Widget *w1, Widget *w2) {
			// 	// If score was not computed, scores[w] returns 0, but this doesn't matter because those widgets aren't visible.
			// 	return get_default(scores, w1, 0.f) > get_default(scores, w2, 0.f);
			// });
		}

		// Filter the brand and tag lists

		// Get modules that would be filtered by just the search query
		std::vector<plugin::Model*> filteredModels;
		for (Widget *w : modelContainer->children) {
			ModelBox *m = dynamic_cast<ModelBox*>(w);
			assert(m);
			if (isModelVisible(m->model, search, "", ""))
				filteredModels.push_back(m->model);
		}

		auto hasModel = [&](const std::string &brand, const std::string &tag) -> bool {
			for (plugin::Model *model : filteredModels) {
				if (isModelVisible(model, "", brand, tag))
					return true;
			}
			return false;
		};

		// Enable brand and tag items that are available in visible ModelBoxes
		int brandsLen = 0;
		for (Widget *w : sidebar->brandList->children) {
			BrandItem *item = dynamic_cast<BrandItem*>(w);
			assert(item);
			item->disabled = !hasModel(item->text, tag);
			if (!item->disabled)
				brandsLen++;
		}
		sidebar->brandLabel->text = string::f("Brands (%d)", brandsLen);

		int tagsLen = 0;
		for (Widget *w : sidebar->tagList->children) {
			TagItem *item = dynamic_cast<TagItem*>(w);
			assert(item);
			item->disabled = !hasModel(brand, item->text);
			if (!item->disabled)
				tagsLen++;
		}
		sidebar->tagLabel->text = string::f("Tags (%d)", tagsLen);

		// Count models
		int modelsLen = 0;
		for (Widget *w : modelContainer->children) {
			if (w->visible)
				modelsLen++;
		}
		modelLabel->text = string::f("Modules (%d)", modelsLen);
	}

	void clear() {
		search = "";
		sidebar->searchField->setText("");
		brand = "";
		tag = "";
		refresh();
	}

	void onShow(const event::Show &e) override {
		refresh();
		OpaqueWidget::onShow(e);
	}
};


// Implementations to resolve dependencies


inline void ModelBox::onButton(const event::Button &e) {
	OpaqueWidget::onButton(e);
	if (e.getTarget() != this)
		return;

	if (e.action == GLFW_PRESS && e.button == GLFW_MOUSE_BUTTON_LEFT) {
		// Hide tooltip
		setTooltip(NULL);

		// Create module
		ModuleWidget *moduleWidget = model->createModuleWidget();
		assert(moduleWidget);
		APP->scene->rack->addModuleAtMouse(moduleWidget);

		// Pretend the moduleWidget was clicked so it can be dragged in the RackWidget
		e.consume(moduleWidget);

		// Hide Module Browser
		BrowserOverlay *overlay = getAncestorOfType<BrowserOverlay>();
		overlay->hide();

		// Push ModuleAdd history action
		history::ModuleAdd *h = new history::ModuleAdd;
		h->name = "create module";
		h->setModule(moduleWidget);
		APP->history->push(h);

		// Step favorite
		stepFavoriteScore(model->plugin->slug, model->slug);
	}
}


inline void BrandItem::onAction(const event::Action &e) {
	ModuleBrowser *browser = getAncestorOfType<ModuleBrowser>();
	if (browser->brand == text)
		browser->brand = "";
	else
		browser->brand = text;
	browser->refresh();
}

inline void BrandItem::step() {
	MenuItem::step();
	ModuleBrowser *browser = getAncestorOfType<ModuleBrowser>();
	active = (browser->brand == text);
}

inline void TagItem::onAction(const event::Action &e) {
	ModuleBrowser *browser = getAncestorOfType<ModuleBrowser>();
	if (browser->tag == text)
		browser->tag = "";
	else
		browser->tag = text;
	browser->refresh();
}

inline void TagItem::step() {
	MenuItem::step();
	ModuleBrowser *browser = getAncestorOfType<ModuleBrowser>();
	active = (browser->tag == text);
}

inline void BrowserSearchField::onChange(const event::Change &e) {
	ModuleBrowser *browser = getAncestorOfType<ModuleBrowser>();
	browser->search = string::trim(text);
	browser->refresh();
}

inline void ClearButton::onAction(const event::Action &e) {
	ModuleBrowser *browser = getAncestorOfType<ModuleBrowser>();
	browser->clear();
}


// Global functions


widget::Widget *moduleBrowserCreate() {
	BrowserOverlay *overlay = new BrowserOverlay;

	ModuleBrowser *browser = new ModuleBrowser;
	overlay->addChild(browser);

	return overlay;
}


} // namespace app
} // namespace rack
