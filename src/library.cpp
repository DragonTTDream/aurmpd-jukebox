#include "library.hpp"

#include <cerrno>
#include <cstdlib>

using json = nlohmann::json;

/*
 * Parse an integer tag without ever throwing. Audio tags are user data: they
 * can be missing, empty, or non-numeric ("", "live", "-", ...), which used to
 * make std::stoi throw std::invalid_argument and terminate the process.
 * Parses the leading digits (so "1/12" -> 1) and falls back to `fallback`
 * when there are none. Keeps the previous field semantics.
 */
static int parseIntOr(const std::string& value, int fallback)
{
    const char *begin = value.c_str();
    char *end = nullptr;

    if (begin == nullptr || *begin == '\0')
        return fallback;

    errno = 0;
    long parsed = std::strtol(begin, &end, 10);
    if (end == begin || errno == ERANGE)
        return fallback;

    return static_cast<int>(parsed);
}

// 定义 Library 类来管理本地歌曲库
//清除历史记录
void Library::clear(){
    for(auto& album : m_albumQueue){
        album.clear();
    } 
    m_albumQueue.clear();
}
// 初始化函数
bool Library::getMpdDB() {
    // The shared mpd connection is also used by the poll thread and by the WS
    // command handler: serialise all libmpdclient access.
    mpd_lock();
    //clear();
    if (!mpd_send_list_all_meta(mpd.conn, nullptr)) {
        std::cerr << "Failed to send list all meta request: " << mpd_connection_get_error_message(mpd.conn) << std::endl;
        mpd_unlock();
        return false;
    }      
    struct mpd_entity* entity;
    while ((entity = mpd_recv_entity(mpd.conn)) != nullptr) {
        if (mpd_entity_get_type(entity) == MPD_ENTITY_TYPE_SONG) {
            const struct mpd_song* song = mpd_entity_get_song(entity);
            const char* title_cstr = mpd_song_get_tag(song, MPD_TAG_TITLE, 0);
            const char* track_cstr = mpd_song_get_tag(song, MPD_TAG_TRACK, 0);
            const char* artist_cstr = mpd_song_get_tag(song, MPD_TAG_ARTIST, 0);
            const char* album_cstr = mpd_song_get_tag(song, MPD_TAG_ALBUM, 0);
            const char* genre_cstr = mpd_song_get_tag(song, MPD_TAG_GENRE, 0);//音乐的风格类别
            unsigned duration = mpd_song_get_duration(song);
            const char* url_cstr = mpd_song_get_uri(song);
            const char* date_cstr = mpd_song_get_tag(song, MPD_TAG_DATE, 0);
            std::string title = title_cstr ? title_cstr : url_cstr;
            std::string track = track_cstr ? track_cstr : "0";
            std::string artist = artist_cstr ? artist_cstr : "unknown";
            std::string album = album_cstr ? album_cstr : "unknown";
            std::string genre = genre_cstr ? genre_cstr : "unknown";
            std::string url = url_cstr ? url_cstr : "unknown";
            std::string date = date_cstr ? date_cstr : "";
            int year = parseIntOr(date.substr(0, date.size() < 4 ? date.size() : 4), 1900);//取前4位，空/非数字回退 1900
            Song newSong(url,artist,title,album,url);
            newSong.setDuration(duration);
            newSong.setTrack(parseIntOr(track, 0));//空/非数字回退 0
            newSong.setYear(year);
            if(!findAlbum(album)){//找不到专辑，新增专辑
                Album newAlbum(album,artist,album,year);
                addAlbum(newAlbum);
            }
            addSong(album,newSong);
        }
        mpd_entity_free(entity);
    }
    mpd_unlock();
    return true;
}

//是否存在某专辑
bool Library::findAlbum(std::string id){
    for (const auto& album : m_albumQueue) {       
        if (album.getId() == id) {
            return true;
        }
    }
    return false;
}
//新增加专辑
bool Library::addAlbum(Album &myalbum){
    /*
    for (const auto& album : m_albumQueue) {
        if (album.getId() == myalbum.getId()) {
            return false;//专辑已经存在
        }
    }
    */
    m_albumQueue.push_back(myalbum);
    return true;
}
//某专辑新增歌曲
bool Library::addSong(std::string id,const Song &song){
    for (auto& album : m_albumQueue) {
        if (album.getId() == id) {
            album.addSong(song);
            return true;
        }
    }
    return false;
}

//所有专辑转json（不包括song）
std::string Library::allAlbumToJson() const{
    json j;
    if(m_albumQueue.size()){
        for (auto& album : m_albumQueue){
            j.push_back(json::parse(album.albumToJson()));
        }
        return j.dump();
    }
    return std::string("[]");//return empty not return null
}

//某专辑的song转json
std::string  Library::AlbumToJson(std::string id) const{
    for (auto& album : m_albumQueue) {
        if (album.getId() == id) {
            return album.toJson();
        }
    }
    return std::string("[]");//return empty not return null
}
// 迭代器相关方法实现
Library::iterator Library::begin() {
    return m_albumQueue.begin();
}

Library::iterator Library::end() {
    return m_albumQueue.end();
}

Library::const_iterator Library::begin() const {
    return m_albumQueue.begin();
}

Library::const_iterator Library::end() const {
    return m_albumQueue.end();
}
