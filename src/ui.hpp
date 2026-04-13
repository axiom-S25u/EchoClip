#include <Geode/Geode.hpp>
#include <Geode/cocos/extensions/GUI/CCScrollView/CCScrollView.h>
#include <filesystem>
#include <vector>
#include <string>

using namespace geode::prelude;
namespace fs = std::filesystem;

std::string format_time_str(fs::file_time_type ft_val);

struct Clip {
    fs::path p_path;
    std::string s_lvl;
    int nAtts;
    std::string s_time_info;
    bool b_is_fav;
};

class Card : public CCNode {
public:
    static Card* create(Clip info, float w, float h);
    bool init(Clip info, float w, float h);
    Clip m_info_struct;
    void onPlay(CCObject* pSender);
    void onDelete(CCObject* pSender);
    void onFavorite(CCObject* pSender);
};

class Gallery : public CCLayerColor, public TextInputDelegate {
public:
    static Gallery* create();
    static void open();
    static void refresh();

    bool init() override;
    void keyBackClicked() override;
    bool ccTouchBegan(CCTouch* p_touch, CCEvent* e_event) override;
    void textChanged(CCTextInputNode* input_node) override;

    CCLayerColor* p_MainPanel;
    CCScrollView* cool_scroller;
    CCLayer* p_inner_container;
    CCLabelBMFont* p_count_label_ptr;
    CCTextInputNode* p_SearchBox;
    std::vector<Clip> v_all_clips;
    std::vector<Clip> v_filtered_list;

    void load();
    void build();
    void onFolder(CCObject* p_obj);
    void onSettings(CCObject* p_obj);
    void onRefresh(CCObject* p_obj);
    void onClear(CCObject* p_obj);
    void onClose(CCObject* p_obj);// if whoever is reviewing this has patience to help with the ui pls do, i did my  best :sob:
};