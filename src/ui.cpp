#include "ui.hpp"
#include <Geode/Geode.hpp>
#include <Geode/cocos/extensions/GUI/CCScrollView/CCScrollView.h>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/utils/general.hpp>
#include <filesystem>
#include <algorithm>
#include <chrono>

#ifdef GEODE_IS_WINDOWS
#include <shellapi.h>
#endif
// please help with the ui if u can, i did my best :sob:
using namespace geode::prelude;
namespace fs = std::filesystem;

std::string format_time_str(fs::file_time_type ft) {
    auto system_tp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
    );
    return geode::utils::timePointAsString(system_tp);
}

Card* Card::create(Clip data, float width_val, float height_val) {
    auto p_node = new Card();
    if (p_node && p_node->init(data, width_val, height_val)) { 
        p_node->autorelease(); 
        return p_node; 
    }
    CC_SAFE_DELETE(p_node); return nullptr;
}

bool Card::init(Clip data_info, float w, float h) {
    if (!CCNode::init()) return false;
    m_info_struct = data_info; 
    setContentSize({w, h});
    
    auto bg_layer = CCLayerColor::create({35, 35, 38, 255}, w, h); 
    addChild(bg_layer);
    
    auto line_sep = CCLayerColor::create({70, 70, 80, 255}, w, 1); 
    line_sep->setPosition(0, 0); 
    addChild(line_sep);
    
    std::string s_display_name = m_info_struct.s_lvl;
    if (s_display_name.size() > 18) s_display_name = s_display_name.substr(0, 16) + "..";
    
    auto lbl_name = CCLabelBMFont::create(s_display_name.c_str(), "bigFont.fnt");
    lbl_name->setScale(0.32f); lbl_name->setAnchorPoint({0, 0.5f}); 
    lbl_name->setPosition(10, h - 14); 
    addChild(lbl_name);
    
    auto lbl_atts = CCLabelBMFont::create(fmt::format("att {}", m_info_struct.nAtts).c_str(), "chatFont.fnt");
    lbl_atts->setScale(0.32f); lbl_atts->setColor({140, 170, 255}); 
    lbl_atts->setAnchorPoint({0, 0.5f}); 
    lbl_atts->setPosition(10, 12); 
    addChild(lbl_atts);
    
    auto lbl_time = CCLabelBMFont::create(m_info_struct.s_time_info.c_str(), "chatFont.fnt");
    lbl_time->setScale(0.26f); lbl_time->setColor({110, 110, 110}); 
    lbl_time->setAnchorPoint({1, 0.5f}); 
    lbl_time->setPosition(w - 70, 12); 
    addChild(lbl_time);
    
    auto p_menu_layer = CCMenu::create(); 
    p_menu_layer->setPosition(0, 0); 
    addChild(p_menu_layer);
    
    auto p_play_spr = CCSprite::createWithSpriteFrameName("GJ_playBtn2_001.png"); 
    p_play_spr->setScale(0.3f);
    auto p_play_btn_obj = CCMenuItemSpriteExtra::create(p_play_spr, this, menu_selector(Card::onPlay));
    p_play_btn_obj->setPosition(w - 52, h / 2 + 2); 
    p_menu_layer->addChild(p_play_btn_obj);
    
    auto p_del_spr = CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png"); 
    p_del_spr->setScale(0.45f);
    auto p_del_btn_obj = CCMenuItemSpriteExtra::create(p_del_spr, this, menu_selector(Card::onDelete));
    p_del_btn_obj->setPosition(w - 13, h / 2); 
    p_menu_layer->addChild(p_del_btn_obj);

    auto fav_spr = CCSprite::create("fav.png"_spr);
    if (!fav_spr) fav_spr = CCSprite::createWithSpriteFrameName("GJ_star_001.png");
    if (!m_info_struct.b_is_fav) fav_spr->setOpacity(80);
    fav_spr->setScale(0.2f);
    auto fav_btn = CCMenuItemSpriteExtra::create(fav_spr, this, menu_selector(Card::onFavorite));
    fav_btn->setPosition(w - 32, h / 2);
    p_menu_layer->addChild(fav_btn);
    
    return true;
}

void Card::onPlay(CCObject*) {
#ifdef GEODE_IS_WINDOWS
    ShellExecuteA(NULL, "open", geode::utils::string::pathToString(m_info_struct.p_path).c_str(), NULL, NULL, SW_SHOWNORMAL);
#endif
}

void Card::onFavorite(CCObject*) {
    std::error_code ec;
    fs::path clips_dir = Mod::get()->getSaveDir() / "clips";
    fs::path new_path;
    
    std::string filename = geode::utils::string::pathToString(m_info_struct.p_path.filename());
    if (m_info_struct.b_is_fav) {
        new_path = clips_dir / m_info_struct.s_lvl / filename;
    } else {
        new_path = clips_dir / "favorites" / m_info_struct.s_lvl / filename;
    }
    
    fs::create_directories(new_path.parent_path(), ec);
    fs::rename(m_info_struct.p_path, new_path, ec);
    Gallery::refresh();
}

void Card::onDelete(CCObject*) {
    fs::path p_path_ptr = m_info_struct.p_path;
    geode::createQuickPopup("Delete Clip", "Delete this clip?", "No", "Yes", [p_path_ptr](auto, bool b_is_yes) {
        if (b_is_yes) { 
            std::error_code ec_err; 
            fs::remove(p_path_ptr, ec_err); 
            Gallery::refresh(); 
        }
    });
}

Gallery* Gallery::create() {
    auto p_obj_ptr = new Gallery();
    if (p_obj_ptr && p_obj_ptr->init()) { 
        p_obj_ptr->autorelease(); 
        return p_obj_ptr; 
    }
    CC_SAFE_DELETE(p_obj_ptr); return nullptr;
}

void Gallery::open() {
    auto s_cur_scene = CCDirector::get()->getRunningScene(); 
    if (!s_cur_scene) return;
    if (CCNode* node_found = s_cur_scene->getChildByID("axiom.echoclip/gallery")) { 
        node_found->removeFromParent(); 
        return; 
    }
    auto p_gal_layer = create(); 
    p_gal_layer->setID("axiom.echoclip/gallery"); 
    s_cur_scene->addChild(p_gal_layer, s_cur_scene->getHighestChildZ() + 1);
}

void Gallery::refresh() {
    auto p_scene_ptr = CCDirector::get()->getRunningScene(); 
    if (!p_scene_ptr) return;
    Gallery* p_g = typeinfo_cast<Gallery*>(p_scene_ptr->getChildByID("axiom.echoclip/gallery"));
    if (p_g) { 
        p_g->load(); 
        p_g->v_filtered_list = p_g->v_all_clips; 
        p_g->build(); 
    }
}

bool Gallery::init() {
    CCSize win_size_val = CCDirector::get()->getWinSize();
    if (!CCLayerColor::initWithColor({0, 0, 0, 180})) return false;
    
    setContentSize(win_size_val); 
    setTouchEnabled(true); 
    setKeypadEnabled(true); 
    setTouchPriority(-500); 
    setTouchMode(kCCTouchesOneByOne);
    
    float f_width = 500.f, f_height = 320.f;
    p_MainPanel = CCLayerColor::create({22, 22, 25, 255}, f_width, f_height);
    p_MainPanel->setPosition(win_size_val.width / 2 - f_width / 2, win_size_val.height / 2 - f_height / 2);
    p_MainPanel->setTouchEnabled(false); 
    addChild(p_MainPanel);
    
    auto top_bar_layer = CCLayerColor::create({38, 38, 45, 255}, f_width, 38); 
    top_bar_layer->setPosition(0, f_height - 38); 
    p_MainPanel->addChild(top_bar_layer);
    
    auto line_acc = CCLayerColor::create({80, 100, 200, 255}, f_width, 2); 
    line_acc->setPosition(0, f_height - 40); 
    p_MainPanel->addChild(line_acc);
    
    auto p_title_label = CCLabelBMFont::create("EchoClip", "bigFont.fnt");
    p_title_label->setScale(0.52f); p_title_label->setPosition({f_width / 2 - 80, f_height - 19}); 
    p_title_label->setColor({240, 240, 240}); 
    p_MainPanel->addChild(p_title_label, 1);
    
    auto p_top_menu = CCMenu::create(); 
    p_top_menu->setPosition({0, 0}); 
    p_MainPanel->addChild(p_top_menu, 10);
    
    auto p_close_spr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png"); 
    p_close_spr->setScale(0.85f);
    auto p_close_btn_obj = CCMenuItemSpriteExtra::create(p_close_spr, this, menu_selector(Gallery::onClose)); 
    p_close_btn_obj->setPosition({16, f_height - 19}); 
    p_top_menu->addChild(p_close_btn_obj);
    
    auto p_ref_spr = CCSprite::createWithSpriteFrameName("GJ_updateBtn_001.png"); 
    p_ref_spr->setScale(0.65f);
    auto p_ref_btn_obj = CCMenuItemSpriteExtra::create(p_ref_spr, this, menu_selector(Gallery::onRefresh)); 
    p_ref_btn_obj->setPosition({f_width - 16, f_height - 19}); 
    p_top_menu->addChild(p_ref_btn_obj);
    
    auto p_search_bg = CCLayerColor::create({12, 12, 14, 255}, 140, 22); 
    p_search_bg->setPosition(f_width - 158, f_height - 30); 
    p_MainPanel->addChild(p_search_bg, 1);
    
    p_SearchBox = CCTextInputNode::create(130, 18, "search...", "chatFont.fnt");
    p_SearchBox->setPosition({f_width - 88, f_height - 19}); 
    p_SearchBox->setDelegate(this); 
    p_SearchBox->setLabelPlaceholderColor({80, 80, 80}); 
    p_MainPanel->addChild(p_SearchBox, 2);
    
    p_inner_container = CCLayer::create(); 
    cool_scroller = CCScrollView::create({f_width - 16, f_height - 78}, p_inner_container);
    cool_scroller->setDirection(kCCScrollViewDirectionVertical); 
    cool_scroller->setPosition({8, 44}); 
    cool_scroller->setTouchPriority(-501); 
    p_MainPanel->addChild(cool_scroller, 1);
    
    load(); 
    v_filtered_list = v_all_clips; 
    build();
    
    auto p_bottom_bg = CCLayerColor::create({30, 30, 35, 255}, f_width, 42); 
    p_bottom_bg->setPosition(0, 0); 
    p_MainPanel->addChild(p_bottom_bg, 0);
    
    auto p_bottom_line = CCLayerColor::create({60, 60, 70, 255}, f_width, 1); 
    p_bottom_line->setPosition(0, 42); 
    p_MainPanel->addChild(p_bottom_line, 1);
    
    auto p_bottom_menu = CCMenu::create(); 
    p_bottom_menu->setPosition({f_width / 2 + 30, 21});
    p_bottom_menu->setLayout(RowLayout::create()->setGap(8)->setAxisAlignment(AxisAlignment::Center)); 
    p_MainPanel->addChild(p_bottom_menu, 10);
    
    p_bottom_menu->addChild(CCMenuItemSpriteExtra::create(ButtonSprite::create("Folder", "goldFont.fnt", "GJ_button_04.png", 0.6f), this, menu_selector(Gallery::onFolder)));
    p_bottom_menu->addChild(CCMenuItemSpriteExtra::create(ButtonSprite::create("Settings", "goldFont.fnt", "GJ_button_04.png", 0.6f), this, menu_selector(Gallery::onSettings)));
    p_bottom_menu->addChild(CCMenuItemSpriteExtra::create(ButtonSprite::create("Clear All", "goldFont.fnt", "GJ_button_06.png", 0.6f), this, menu_selector(Gallery::onClear)));
    p_bottom_menu->updateLayout();
    
    p_count_label_ptr = CCLabelBMFont::create("0 clips", "chatFont.fnt");
    p_count_label_ptr->setScale(0.38f); p_count_label_ptr->setColor({100, 100, 100});
    p_count_label_ptr->setAnchorPoint({0, 0.5f}); 
    p_count_label_ptr->setPosition({10, 21}); 
    p_MainPanel->addChild(p_count_label_ptr, 1);
    
    if (p_count_label_ptr) p_count_label_ptr->setString(fmt::format("{} clips", v_filtered_list.size()).c_str());
    
    return true;
}

void Gallery::textChanged(CCTextInputNode* p_inp) {
    std::string s_query = p_inp->getString();
    if (s_query.empty()) v_filtered_list = v_all_clips;
    else { 
        v_filtered_list.clear(); 
        std::string s_low = geode::utils::string::toLower(s_query); 
        for (auto& c : v_all_clips) {
            if (geode::utils::string::toLower(c.s_lvl).find(s_low) != std::string::npos || 
                geode::utils::string::toLower(geode::utils::string::pathToString(c.p_path.stem())).find(s_low) != std::string::npos) {
                v_filtered_list.push_back(c);
            }
        }
    }
    build(); 
    if (p_count_label_ptr) p_count_label_ptr->setString(fmt::format("{} clips", v_filtered_list.size()).c_str());
}

void Gallery::build() {
    if (!cool_scroller || !p_inner_container) return; 
    p_inner_container->removeAllChildren();
    
    float f_w_in = cool_scroller->getViewSize().width; 
    float pad_val = 2; 
    float card_h = 44; 
    float header_h = 22;
    int count_val = (int)v_filtered_list.size();
    
    if (count_val == 0) {
        auto p_error_lbl = CCLabelBMFont::create("no clips yet? go beat a lvl or smh", "chatFont.fnt"); 
        p_error_lbl->setScale(0.5f); p_error_lbl->setColor({80, 80, 80}); 
        p_error_lbl->setPosition({f_w_in / 2, cool_scroller->getViewSize().height / 2});
        p_inner_container->setContentSize(cool_scroller->getViewSize()); 
        p_inner_container->addChild(p_error_lbl); 
        cool_scroller->setContentSize(cool_scroller->getViewSize()); 
        cool_scroller->setContentOffset({0, 0}); 
        return;
    }
    
    float total_h = 0;
    std::string last_lvl = "";
    int row_in_group = 0;

    for (auto& clip : v_filtered_list) {
        if (clip.s_lvl != last_lvl) {
            last_lvl = clip.s_lvl;
            total_h += header_h + pad_val;
            row_in_group = 0;
        }
        if (row_in_group % 2 == 0) total_h += card_h + pad_val;
        row_in_group++;
    }

    total_h = std::max(cool_scroller->getViewSize().height, total_h + pad_val);
    p_inner_container->setContentSize({f_w_in, total_h});

    float cur_y = total_h - pad_val;
    last_lvl = "";
    row_in_group = 0;

    for (int i = 0; i < count_val; i++) {
        auto& clip = v_filtered_list[i];
        if (clip.s_lvl != last_lvl) {
            if (i > 0 && row_in_group % 2 != 0) cur_y -= card_h + pad_val;
            last_lvl = clip.s_lvl;
            cur_y -= header_h;
            
            auto bar = CCLayerColor::create({45, 45, 50, 255}, f_w_in, header_h);
            bar->setPosition({0, cur_y});
            p_inner_container->addChild(bar);
            
            auto lbl = CCLabelBMFont::create(last_lvl.c_str(), "goldFont.fnt");
            lbl->setScale(0.40f); lbl->setAnchorPoint({0, 0.5f});
            lbl->setPosition({10, header_h / 2});
            bar->addChild(lbl);
            
            cur_y -= pad_val;
            row_in_group = 0;
        }

        float card_w = (f_w_in - pad_val) / 2;
        auto box = Card::create(clip, card_w, card_h);
        float x = (row_in_group % 2) * (card_w + pad_val);
        float y = cur_y - (row_in_group / 2 + 1) * (card_h + pad_val) + pad_val;
        box->setPosition({x, y});
        p_inner_container->addChild(box);

        row_in_group++;
        if (i == count_val - 1 || v_filtered_list[i+1].s_lvl != last_lvl) {
            cur_y -= ((row_in_group + 1) / 2) * (card_h + pad_val);
        }
    }

    cool_scroller->setContentSize({f_w_in, total_h}); 
    cool_scroller->setContentOffset({0, cool_scroller->getViewSize().height - total_h});
}

void Gallery::load() {
    v_all_clips.clear(); 
    std::error_code ec;
    fs::path d_path_obj = Mod::get()->getSaveDir() / "clips"; 
    if (!fs::exists(d_path_obj, ec)) return;
    
    for (auto& entry_ptr : fs::recursive_directory_iterator(d_path_obj, ec)) {
        if (ec) break;
        if (!entry_ptr.is_regular_file(ec) || (entry_ptr.path().extension() != ".mkv" && entry_ptr.path().extension() != ".mp4")) continue;
        
        std::string s_name_stem = geode::utils::string::pathToString(entry_ptr.path().stem()); 
        Clip c_info; 
        c_info.p_path = entry_ptr.path(); 
        c_info.nAtts = 0; 
        
        std::string rel = geode::utils::string::pathToString(fs::relative(c_info.p_path, d_path_obj, ec));
        c_info.b_is_fav = rel.find("favorites") != std::string::npos;
        
        c_info.s_lvl = geode::utils::string::pathToString(entry_ptr.path().parent_path().filename());
        if (c_info.s_lvl == "clips" || c_info.s_lvl == "favorites") c_info.s_lvl = s_name_stem;
        
        size_t pos_found = s_name_stem.rfind("_att");
        if (pos_found != std::string::npos) {
            std::string s_num_rest = s_name_stem.substr(pos_found + 4); 
            size_t idx_u = s_num_rest.rfind('_');
            std::string s_final_num = (idx_u != std::string::npos) ? s_num_rest.substr(0, idx_u) : s_num_rest;
            bool is_digit_ok = true; 
            for (size_t j = 0; j < s_final_num.size(); j++) if (!isdigit(s_final_num[j])) { is_digit_ok = false; break; }
            if (is_digit_ok && !s_final_num.empty()) c_info.nAtts = std::atoi(s_final_num.c_str());
        }
        c_info.s_time_info = format_time_str(fs::last_write_time(entry_ptr.path(), ec)); 
        v_all_clips.push_back(c_info);
    }
    
    std::sort(v_all_clips.begin(), v_all_clips.end(), [](Clip& a, Clip& b) { 
        if (a.s_lvl != b.s_lvl) return a.s_lvl < b.s_lvl;
        std::error_code ec1, ec2;
        return fs::last_write_time(a.p_path, ec1) > fs::last_write_time(b.p_path, ec2); 
    });
}


void Gallery::onFolder(CCObject*) {
    geode::utils::file::openFolder(Mod::get()->getSaveDir() / "clips");
}

void Gallery::onSettings(CCObject*) { geode::openSettingsPopup(Mod::get()); }
void Gallery::onRefresh(CCObject* p_unused) { 
    load(); 
    v_filtered_list = v_all_clips; 
    build(); 
    if (p_count_label_ptr) p_count_label_ptr->setString(fmt::format("{} clips", v_filtered_list.size()).c_str()); 
}

void Gallery::onClear(CCObject*) {
    geode::createQuickPopup("Clear All", "Delete all non-favorite clips?\nThis can't be undone.", "No", "Yes", [this](auto, bool b_sure) { // sorry if ui is shit i did my best
        if (b_sure) { 
            std::error_code ec;
            fs::path clips_dir = Mod::get()->getSaveDir() / "clips";
            if (fs::exists(clips_dir, ec)) {
                for (auto const& entry : fs::directory_iterator(clips_dir, ec)) {
                    auto name = geode::utils::string::pathToString(entry.path().filename());
                    if (name != "favorites" && name != "temp") {
                        fs::remove_all(entry.path(), ec);
                    }
                }
            }
            onRefresh(nullptr); 
        } 
    });
}

void Gallery::onClose(CCObject*) { removeFromParent(); }
void Gallery::keyBackClicked() { removeFromParent(); }
// if whoever is reviewing this has patience to help with the ui pls do, i did my  best :sob:, pls make a pr
bool Gallery::ccTouchBegan(CCTouch* p_t, CCEvent*) {
    CCPoint loc_pt = p_MainPanel->convertTouchToNodeSpace(p_t); 
    CCSize panel_sz = p_MainPanel->getContentSize();
    if (loc_pt.x < 0 || loc_pt.x > panel_sz.width || loc_pt.y < 0 || loc_pt.y > panel_sz.height) removeFromParent();
    return true;
}
