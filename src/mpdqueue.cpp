#include "mpdqueue.hpp"

#include <cerrno>
#include <cstdlib>
#include <vector>

using json = nlohmann::json;

// 解析 tag 的前导数字；音频 tag 是用户数据，可能为空或非数字（"live"、"1/12"），
// 用 strtol 而不是 std::stoi，避免抛异常导致进程终止。
static int parseLeadingInt(const char *value, int fallback) {
    if (value == nullptr || *value == '\0')
        return fallback;
    errno = 0;
    char *end = nullptr;
    long parsed = std::strtol(value, &end, 10);
    if (end == value || errno == ERANGE)
        return fallback;
    return static_cast<int>(parsed);
}

// 定义 Mpdqueue 类来管理mpd列表
// 添加歌曲到队列末尾
int Mpdqueue::addSong(Song& song) {
    // 发送addid命令并获取歌曲queue_id
    mpd_lock();
    int queue_sid = mpd_run_add_id(mpd.conn, song.getURL().c_str());
    if (queue_sid < 0) {
        std::cerr<<"Failed to add song:"<<mpd_connection_get_error_message(mpd.conn)<<" song.url="<<song.getURL().c_str()<<std::endl;
        mpd_unlock();
        return -1;
    }else{
        mpd_unlock();
        song.setQueueSId(queue_sid);//add queue id
        Queue::addSong(song);
        return queue_sid;
    }
}

// 通过查询queue根据id补全queue队列的pos信息
void Mpdqueue::makeupPos(){
    mpd_lock();
    if (!mpd_send_list_queue_meta(mpd.conn)) {
        std::cerr << "Get queue failed: " << mpd_connection_get_error_message(mpd.conn) << std::endl;
        mpd_unlock();
        return;
    }    
    mpd_song *song;
    while ((song = mpd_recv_song(mpd.conn)) != nullptr) {
        unsigned int position = mpd_song_get_pos(song);
        unsigned int sid = mpd_song_get_id(song);
        std::optional<Song> m_song = getSongBySId(sid);
        if(m_song){
            (*m_song).setPos(position);
            updateSonge(*m_song);
        }
        mpd_song_free(song);
    }
    if (mpd_connection_get_error(mpd.conn) != MPD_ERROR_SUCCESS) {
        std::cerr << "recive queue failed: " << mpd_connection_get_error_message(mpd.conn) << std::endl;
    }
    mpd_unlock();
    //mpd_response_finish(mpd.conn);
}

// 清空歌曲队列
void Mpdqueue::clear() {
    mpd_lock();
    mpd_run_clear(mpd.conn);
    mpd_unlock();
    Queue::clear();
}

////播放pos位置的歌曲
void Mpdqueue::playPos(int pos){
    if(getSongByPos(pos)){
        mpd_lock();
        mpd_run_play_pos(mpd.conn,pos);
        mpd_unlock();
    }
}

//根据queue_sid播放歌曲
void Mpdqueue::playSid(int queue_sid){
    if(getSongBySId(queue_sid)){
        mpd_lock();
        mpd_run_play_id(mpd.conn,queue_sid);
        mpd_unlock();
    }
}

// 删除指定 queue_sid 的歌曲
bool Mpdqueue::removeSongBySid(const int queue_sid) {
    if(Queue::removeSongBySid(queue_sid)){
        mpd_lock();
        bool ok = mpd_run_delete_id(mpd.conn, queue_sid);
        if (!ok) {
            std::cerr << "Failed to delete song from MPD: " << mpd_connection_get_error_message(mpd.conn) << std::endl;
            // 这里可以考虑恢复本地队列，避免数据不一致
        }
        mpd_unlock();
        return ok;
    }
    return false;
}

/*
 * 用 mpd 当前播放队列重建内部队列。
 *
 * 这是队列一致性的关键：mpd 是队列/播放状态的唯一真相，内部队列只是它的镜像。
 * 之前内部队列是纯内存态，只在 /api/queue/add|replace|del 时才和 mpd 同步，
 * 因此 aurmpd 一重启（或换一个客户端打开），GET /api/queue 就返回空，前端
 * 看不到队列、queue_sid 也对不上 mpd 的歌曲，用户表现为「失去记录、无法控制」。
 *
 * queue_sid 方案：直接使用 mpd 的 song id（mpd_song_get_id()）。这与
 * addSong() 里 mpd_run_add_id() 的返回值本来就是同一语义，所以不需要任何
 * id 映射；重启、多客户端、外部用 mpc 改动都能天然对上。
 *
 * 锁：调用方（mpd_poll）已持有 mpd_lock；mpd_lock 是递归锁，这里再拿一次
 * 是为了让 HTTP 线程也能安全调用，同时避免与 toJson() 的读并发。
 */
void Mpdqueue::syncFromMpd() {
    mpd_lock();
    if (mpd.conn == nullptr || mpd.conn_state != MPD_CONNECTED) {
        mpd_unlock();
        return;
    }

    if (!mpd_send_list_queue_meta(mpd.conn)) {
        std::cerr << "syncFromMpd: list queue failed: "
                  << mpd_connection_get_error_message(mpd.conn) << std::endl;
        mpd_connection_clear_error(mpd.conn);
        mpd_unlock();
        return;
    }

    std::vector<Song> rebuilt;
    struct mpd_song *song;
    while ((song = mpd_recv_song(mpd.conn)) != nullptr) {
        const char *uri = mpd_song_get_uri(song);
        const char *title = mpd_song_get_tag(song, MPD_TAG_TITLE, 0);
        const char *artist = mpd_song_get_tag(song, MPD_TAG_ARTIST, 0);
        const char *album = mpd_song_get_tag(song, MPD_TAG_ALBUM, 0);
        const char *track = mpd_song_get_tag(song, MPD_TAG_TRACK, 0);
        const char *date = mpd_song_get_tag(song, MPD_TAG_DATE, 0);
        int sid = mpd_song_get_id(song);
        unsigned int pos = mpd_song_get_pos(song);

        // 运行期增量添加过的条目内部已带有前端给的 subsonic id / coverArt 等
        // 字段，mpd 里没有；同 queue_sid 时保留原条目，只刷新 pos。
        std::optional<Song> existing = getSongBySId(sid);
        if (existing) {
            (*existing).setPos(pos);
            rebuilt.push_back(*existing);
        } else {
            std::string uri_str = uri ? uri : "";
            std::string s_id = uri_str;
            std::string s_title = title ? title : uri_str;
            std::string s_artist = artist ? artist : "unknown";
            std::string s_album = album ? album : "unknown";
            std::string s_url = uri_str;
            Song s(s_id, s_artist, s_title, s_album, s_url);
            s.setQueueSId(sid);
            s.setPos(pos);
            s.setDuration(static_cast<int>(mpd_song_get_duration(song)));
            s.setTrack(parseLeadingInt(track, 0));
            s.setYear(parseLeadingInt(date, 1900));
            rebuilt.push_back(s);
        }
        mpd_song_free(song);
    }

    if (mpd_connection_get_error(mpd.conn) != MPD_ERROR_SUCCESS ||
        !mpd_response_finish(mpd.conn)) {
        std::cerr << "syncFromMpd: recv queue failed: "
                  << mpd_connection_get_error_message(mpd.conn) << std::endl;
        mpd_connection_clear_error(mpd.conn);
        mpd_unlock();
        return; // 保畕旧队列，不用半成品覆盖
    }

    Queue::clear();
    for (auto &s : rebuilt)
        Queue::addSong(s);
    mpd_unlock();
}
