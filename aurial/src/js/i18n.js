/*
 * Lightweight i18n for the aurial frontend.
 *
 * - Two built-in dictionaries: `en` (default + fallback) and `zh-CN`.
 * - `t(key, params)` resolves the key in the active language, falls back to
 *   English, then to the key itself, so a missing entry never renders blank.
 *   `{name}` placeholders in an entry are replaced from `params`.
 * - The active language is persisted in localStorage and, on a first visit,
 *   guessed from navigator.language (anything `zh*` maps to zh-CN, else en).
 * - `onLanguageChange(fn)` lets the UI re-render immediately on a switch.
 */

export const STORAGE_KEY = 'lang';

const en = {
	'app.title': 'Jukebox',
	'app.github': 'Aurmpd on GitHub',
	'app.upstream': 'Upstream',
	'app.thisRepo': 'This fork',
	'settings.about': 'About',
	'settings.basedOn': 'Based on',
	'settings.version': 'Version',
	'settings.license': 'License',
	'settings.repoNote': 'This build is a modified version of the upstream project above; the source for this fork lives in the second repository. Both are GPL-2.0.',
	'app.tab.selection': 'Selection',
	'app.tab.playlists': 'Playlists',
	'app.tab.queue': 'Queue',
	'app.tab.settings': 'Settings',

	'common.question': 'Question',
	'common.areYouSure': 'Are you sure?',
	'common.ok': 'OK',
	'common.cancel': 'Cancel',
	'common.yes': 'Yes',
	'common.no': 'No',
	'common.selectOption': 'Select an option...',
	'common.prompt': 'Prompt',
	'common.provideValue': 'Please provide a value',
	'common.view': 'View',
	'imageViewer.title': 'Image Viewer',

	'player.nothingPlaying': 'Nothing playing',
	'player.repeatQueue': 'Repeat queue',
	'player.repeatSingle': 'Repeat single track',
	'player.consume': 'Consume (remove tracks from queue after playing)',
	'player.playingCount': 'Playing {count} track(s).',
	'player.queueCleared': 'Queue cleared.',
	'player.addedCount': 'Added {count} track(s) to the queue.',
	'player.addedPlayingCount': 'Added {count} track(s) and playing.',
	'player.queueUpdateFailed': 'Queue update failed: {error}',
	'player.seekHint': 'Click or drag to seek',
	'player.seekUnavailable': 'Seeking unavailable: no current track or unknown duration',

	'tracklist.number': '#',
	'tracklist.artist': 'Artist',
	'tracklist.title': 'Title',
	'tracklist.album': 'Album',
	'tracklist.date': 'Date',
	'tracklist.duration': 'Duration',
	'tracklist.time': 'Time',
	'tracklist.playNow': 'Play now',
	'tracklist.addToQueue': 'Add to queue',
	'tracklist.removeFromQueue': 'Remove from queue',
	'tracklist.addToPlaylist': 'Add to playlist',
	'tracklist.removeFromPlaylist': 'Remove from playlist',

	'selection.nothingHeader': 'Nothing Selected!',
	'selection.nothingMessage': 'Select an album from the browser.',
	'selection.play': 'Play',
	'selection.addToQueue': 'Add to Queue',
	'selection.addToPlaylist': 'Add to Playlist',
	'selection.year': 'Year: {year}',
	'selection.added': 'Added: {date}',
	'selection.updated': 'Updated: {date}',
	'selection.tracks': '{count} tracks, {duration}',

	'queue.nothingHeader': 'Nothing in the queue!',
	'queue.nothingMessage': 'Add some tracks to the queue by browsing, or selecting a playlist.',
	'queue.summary': '{count} tracks, {duration}',
	'queue.clear': 'Clear Queue',

	'browser.library': 'Library',
	'browser.search': 'Search...',
	'browser.searchLibrary': 'Search library...',
	'browser.selectAll': 'Select all',
	'browser.addSelected': 'Add selected ({count})',
	'browser.scanLibrary': 'Scan library',
	'browser.addSelectedTitle': 'Add selected albums to queue',
	'browser.scanTitle': 'Ask mpd to rescan the music library',
	'browser.play': 'Play',
	'browser.queue': 'Queue',
	'browser.playAll': 'Play all',
	'browser.addAllToQueue': 'Add all tracks to queue',
	'browser.tracks': '{count} tracks',
	'browser.failedArtists': 'Failed to load artists. Check settings.',
	'browser.unableArtists': 'Unable to get artists: {error}',
	'browser.unableArtistAlbums': "Unable to load artist's albums: {error}",
	'browser.unableAlbum': 'Unable to load album: {error}',
	'browser.unableLibrary': 'Unable to load library: {error}',
	'browser.noTracks': 'Album has no tracks.',
	'browser.unablePlayAlbum': 'Unable to play album: {error}',
	'browser.unableAddAlbum': 'Unable to add album to queue: {error}',
	'browser.selectFirst': 'Select at least one album first.',
	'browser.selectedNoTracks': 'Selected albums have no tracks.',
	'browser.unableAddSelection': 'Unable to add selection to queue: {error}',
	'browser.scanRequested': 'Scan requested. A full scan can take a while - refresh the page when it finishes.',
	'browser.matchingTracks': 'Matching tracks ({count})',
	'browser.noResults': 'No matching tracks or albums for "{query}".',
	'browser.searchingTracks': 'Searching tracks...',
	'browser.playTrack': 'Play this track',
	'browser.queueTrack': 'Add this track to the queue',
	'browser.clearSearch': 'Clear search',
	'browser.expandTracks': 'Show tracks',
	'browser.collapseTracks': 'Hide tracks',
	'browser.openInSelection': 'Open in Selection',

	'settings.language': 'Language',
	'settings.connection': 'Subsonic Connection',
	'settings.url': 'Subsonic URL',
	'settings.username': 'Username',
	'settings.password': 'Password',
	'settings.passwordHint': 'leave blank to keep unchanged',
	'settings.preferences': 'Preferences',
	'settings.bufferLabel': 'Buffer next track (begin buffering this long before end of the current track)',
	'settings.bufferDisabled': 'Disabled',
	'settings.buffer10': '10 seconds',
	'settings.buffer30': '30 seconds',
	'settings.notifications': 'Enable desktop notifications',
	'settings.backgroundArt': 'Enable background art',
	'settings.autostart': 'Auto start on login',
	'settings.autostartFailed': 'Could not change auto start setting.',
	'settings.save': 'Save',
	'settings.demo': 'Demo Server',
	'settings.test': 'Test Connection',
	'settings.saved': 'Settings saved.',
	'settings.testSuccess': 'Connection test successful!',
	'settings.testFailed': 'Failed to connect to server: {error}',
	'settings.demoTitle': 'Use Demo Server',
	'settings.demoMessage': 'Reconfigure to use the Subsonic demo server? Please see http://www.subsonic.org/pages/demo.jsp for more information.',
	'settings.yes': 'Yes',
	'settings.no': 'No',

	'playlist.nothingHeader': 'Nothing Selected!',
	'playlist.nothingMessage': 'Select a playlist.',
	'playlist.emptyHeader': 'Empty Playlist',
	'playlist.emptyMessage': 'This playlist has no tracks.',
	'playlist.useLocal': 'Using local (mpd) playlists - use the Playlists tab to load or delete one.',
	'playlist.deleted': 'Playlist deleted',
	'playlist.renamed': 'Playlist renamed',
	'playlist.created': 'New playlist {name} created',
	'playlist.updated': 'Playlist updated',
	'playlist.unableLoad': 'Unable to load playlist: {error}',
	'playlist.unableGet': 'Unable to get playlists: {error}',
	'playlist.createFailed': 'Failed to create playlist: {error}',
	'playlist.updateFailed': 'Failed to update playlist: {error}',
	'playlist.mpdError': 'MPD error: {error}',
	'playlist.unknownError': 'unknown error',
	'playlist.enterName': 'Please enter a playlist name',
	'playlist.saving': 'Saving current queue as {name}...',
	'playlist.loading': 'Loading {name} (this replaces the current queue)...',
	'playlist.createTitle': 'Create Playlist',
	'playlist.renameTitle': 'Rename Playlist',
	'playlist.deleteTitle': 'Delete Playlist',
	'playlist.deleteMessage': 'Are you sure you want to delete this playlist?',
	'playlist.addTitle': 'Add to playlist',
	'playlist.chooseTitle': 'Choose a playlist to add tracks to',
	'playlist.saveQueueTitle': 'Save Current Queue',
	'playlist.enterNameMessage': 'Enter a name for the new playlist',
	'playlist.enterNewNameMessage': 'Enter a new name for this playlist',
	'playlist.playlistsPlaceholder': 'Playlists...',
	'playlist.newPlaylist': 'New Playlist',
	'playlist.play': 'Play',
	'playlist.addToQueue': 'Add to Queue',
	'playlist.rename': 'Rename',
	'playlist.delete': 'Delete',
	'playlist.refresh': 'Refresh',
	'playlist.saveQueue': 'Save current queue',
	'playlist.loadToQueue': 'Load to queue',
	'playlist.add': 'Add',
	'playlist.save': 'Save',

	'errors.scrobbleFailed': 'Scrobble failed for track {title}',
	'errors.httpFailed': 'HTTP request failed, status {status}',
	'errors.httpStatus': 'HTTP request failed, status: {status}',
	'errors.timeout': 'Request timed out after {seconds} seconds'
};

const zhCN = {
	'app.title': '点歌机',
	'app.github': 'GitHub 上的 Aurmpd',
	'app.upstream': '上游',
	'app.thisRepo': '本仓库',
	'settings.about': '关于',
	'settings.basedOn': '基于',
	'settings.version': '版本',
	'settings.license': '许可',
	'settings.repoNote': '本项目是上面「上游」仓库的修改版；本分支的源码在第二个仓库里。两者均为 GPL-2.0。',
	'app.tab.selection': '选中',
	'app.tab.playlists': '歌单',
	'app.tab.queue': '队列',
	'app.tab.settings': '设置',

	'common.question': '提示',
	'common.areYouSure': '确定吗？',
	'common.ok': '确定',
	'common.cancel': '取消',
	'common.yes': '是',
	'common.no': '否',
	'common.selectOption': '请选择一项…',
	'common.prompt': '输入',
	'common.provideValue': '请输入内容',
	'common.view': '查看',
	'imageViewer.title': '图片查看',

	'player.nothingPlaying': '未在播放',
	'player.repeatQueue': '循环整个队列',
	'player.repeatSingle': '单曲循环',
	'player.consume': '消费模式（播完即从队列移除）',
	'player.playingCount': '正在播放 {count} 首。',
	'player.queueCleared': '已清空队列。',
	'player.addedCount': '已添加 {count} 首到队列。',
	'player.addedPlayingCount': '已添加 {count} 首并开始播放。',
	'player.queueUpdateFailed': '队列更新失败：{error}',
	'player.seekHint': '点击或拖动进度条跳转',
	'player.seekUnavailable': '当前无法跳转：没有正在播放的曲目或时长未知',

	'tracklist.number': '#',
	'tracklist.artist': '艺术家',
	'tracklist.title': '标题',
	'tracklist.album': '专辑',
	'tracklist.date': '日期',
	'tracklist.duration': '时长',
	'tracklist.time': '时长',
	'tracklist.playNow': '立即播放',
	'tracklist.addToQueue': '加入队列',
	'tracklist.removeFromQueue': '移出队列',
	'tracklist.addToPlaylist': '加入歌单',
	'tracklist.removeFromPlaylist': '移出歌单',

	'selection.nothingHeader': '未选中任何内容',
	'selection.nothingMessage': '请从左侧曲库中选择一张专辑。',
	'selection.play': '播放',
	'selection.addToQueue': '加入队列',
	'selection.addToPlaylist': '加入歌单',
	'selection.year': '年份：{year}',
	'selection.added': '加入时间：{date}',
	'selection.updated': '更新时间：{date}',
	'selection.tracks': '共 {count} 首，{duration}',

	'queue.nothingHeader': '队列是空的',
	'queue.nothingMessage': '从曲库中挑几首歌，或直接载入一张歌单。',
	'queue.summary': '共 {count} 首，{duration}',
	'queue.clear': '清空队列',

	'browser.library': '本地曲库',
	'browser.search': '搜索…',
	'browser.searchLibrary': '搜索曲库…',
	'browser.selectAll': '全选',
	'browser.addSelected': '加入所选（{count}）',
	'browser.scanLibrary': '扫描曲库',
	'browser.addSelectedTitle': '把所选专辑加入队列',
	'browser.scanTitle': '让 mpd 重新扫描音乐库',
	'browser.play': '播放',
	'browser.queue': '入队',
	'browser.playAll': '全部播放',
	'browser.addAllToQueue': '把这张专辑全部加入队列',
	'browser.tracks': '{count} 首',
	'browser.failedArtists': '艺术家加载失败，请检查设置。',
	'browser.unableArtists': '无法获取艺术家列表：{error}',
	'browser.unableArtistAlbums': '无法加载该艺术家的专辑：{error}',
	'browser.unableAlbum': '无法加载专辑：{error}',
	'browser.unableLibrary': '无法加载本地曲库：{error}',
	'browser.noTracks': '该专辑没有曲目。',
	'browser.unablePlayAlbum': '无法播放专辑：{error}',
	'browser.unableAddAlbum': '无法把专辑加入队列：{error}',
	'browser.selectFirst': '请先勾选至少一张专辑。',
	'browser.selectedNoTracks': '所选专辑没有曲目。',
	'browser.unableAddSelection': '无法把所选加入队列：{error}',
	'browser.scanRequested': '已请求扫描曲库：全量扫描可能耗时较长，完成后刷新页面即可看到新曲目。',
	'browser.matchingTracks': '匹配曲目（{count}）',
	'browser.noResults': '没有匹配“{query}”的曲目或专辑。',
	'browser.searchingTracks': '正在搜索曲目…',
	'browser.playTrack': '播放这首',
	'browser.queueTrack': '把这首加入队列',
	'browser.clearSearch': '清空搜索',
	'browser.expandTracks': '展开曲目',
	'browser.collapseTracks': '收起曲目',
	'browser.openInSelection': '在选中页打开',

	'settings.language': '语言',
	'settings.connection': 'Subsonic 连接',
	'settings.url': 'Subsonic 地址',
	'settings.username': '用户名',
	'settings.password': '密码',
	'settings.passwordHint': '留空表示不修改',
	'settings.preferences': '偏好设置',
	'settings.bufferLabel': '预缓冲下一首（在当前曲目结束前多久开始缓冲）',
	'settings.bufferDisabled': '关闭',
	'settings.buffer10': '10 秒',
	'settings.buffer30': '30 秒',
	'settings.notifications': '启用桌面通知',
	'settings.backgroundArt': '启用背景封面',
	'settings.autostart': '开机自启',
	'settings.autostartFailed': '无法修改开机自启设置。',
	'settings.save': '保存',
	'settings.demo': '演示服务器',
	'settings.test': '测试连接',
	'settings.saved': '设置已保存。',
	'settings.testSuccess': '连接测试成功！',
	'settings.testFailed': '连接服务器失败：{error}',
	'settings.demoTitle': '使用演示服务器',
	'settings.demoMessage': '要改用 Subsonic 演示服务器吗？详见 http://www.subsonic.org/pages/demo.jsp',
	'settings.yes': '是',
	'settings.no': '否',

	'playlist.nothingHeader': '未选中任何内容',
	'playlist.nothingMessage': '请选择一张歌单。',
	'playlist.emptyHeader': '空歌单',
	'playlist.emptyMessage': '这张歌单里没有曲目。',
	'playlist.useLocal': '当前使用本地（mpd）歌单 —— 在「歌单」标签页里加载或删除。',
	'playlist.deleted': '歌单已删除',
	'playlist.renamed': '歌单已重命名',
	'playlist.created': '已创建新歌单 {name}',
	'playlist.updated': '歌单已更新',
	'playlist.unableLoad': '无法加载歌单：{error}',
	'playlist.unableGet': '无法获取歌单列表：{error}',
	'playlist.createFailed': '创建歌单失败：{error}',
	'playlist.updateFailed': '更新歌单失败：{error}',
	'playlist.mpdError': 'MPD 错误：{error}',
	'playlist.unknownError': '未知错误',
	'playlist.enterName': '请输入歌单名称',
	'playlist.saving': '正在把当前队列保存为 {name}…',
	'playlist.loading': '正在加载 {name}（会替换当前队列）…',
	'playlist.createTitle': '创建歌单',
	'playlist.renameTitle': '重命名歌单',
	'playlist.deleteTitle': '删除歌单',
	'playlist.deleteMessage': '确定要删除这张歌单吗？',
	'playlist.addTitle': '加入歌单',
	'playlist.chooseTitle': '选择要加入的歌单',
	'playlist.saveQueueTitle': '保存当前队列',
	'playlist.enterNameMessage': '给新歌单起个名字',
	'playlist.enterNewNameMessage': '输入这张歌单的新名字',
	'playlist.playlistsPlaceholder': '歌单…',
	'playlist.newPlaylist': '新建歌单',
	'playlist.play': '播放',
	'playlist.addToQueue': '加入队列',
	'playlist.rename': '重命名',
	'playlist.delete': '删除',
	'playlist.refresh': '刷新',
	'playlist.saveQueue': '保存当前队列',
	'playlist.loadToQueue': '载入队列',
	'playlist.add': '加入',
	'playlist.save': '保存',

	'errors.scrobbleFailed': '曲目 {title} 上报失败',
	'errors.httpFailed': 'HTTP 请求失败，状态 {status}',
	'errors.httpStatus': 'HTTP 请求失败，状态码 {status}',
	'errors.timeout': '请求超过 {seconds} 秒未响应，已超时'
};

const dictionaries = { 'en': en, 'zh-CN': zhCN };

export const languages = [
	{code: 'en', label: 'English'},
	{code: 'zh-CN', label: '中文（简体）'}
];

function normalize(lang) {
	if (!lang) return 'en';
	return String(lang).toLowerCase().indexOf('zh') === 0 ? 'zh-CN' : 'en';
}

function readStored() {
	try {
		var stored = window.localStorage.getItem(STORAGE_KEY);
		if (stored) return normalize(stored);
	} catch (e) { /* localStorage unavailable (private mode, jsdom, ...) */ }
	try {
		return normalize(navigator.language || navigator.userLanguage);
	} catch (e) {
		return 'en';
	}
}

let current = readStored();
const listeners = [];

export function getLanguage() {
	return current;
}

export function setLanguage(lang) {
	var next = normalize(lang);
	if (next === current) return next;

	current = next;
	try { window.localStorage.setItem(STORAGE_KEY, next); } catch (e) { /* ignore */ }

	listeners.slice().forEach(function(listener) {
		try { listener(next); } catch (e) { console.error('i18n: language listener failed', e); }
	});
	return next;
}

export function onLanguageChange(listener) {
	listeners.push(listener);
	return listener;
}

export function offLanguageChange(listener) {
	var i = listeners.indexOf(listener);
	if (i !== -1) listeners.splice(i, 1);
}

/**
* Translate `key` for the active language. Missing keys fall back to English
* and then to the key itself. `{name}` placeholders are filled from `params`.
*/
export function t(key, params) {
	var value = dictionaries[current] ? dictionaries[current][key] : undefined;
	if (value == null) value = en[key];
	if (value == null) return String(key);

	if (params) {
		Object.keys(params).forEach(function(name) {
			var replacement = params[name];
			value = value.split('{' + name + '}').join(replacement == null ? '' : String(replacement));
		});
	}
	return value;
}

export default {
	t: t,
	languages: languages,
	getLanguage: getLanguage,
	setLanguage: setLanguage,
	onLanguageChange: onLanguageChange,
	offLanguageChange: offLanguageChange
};
