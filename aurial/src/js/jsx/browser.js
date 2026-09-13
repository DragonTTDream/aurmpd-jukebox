import { h, Component } from 'preact';
import {UniqueID} from '../util'
import {IconMessage,CoverArt} from './common'
import {Messages} from './app'
import {sendCommand} from '../mpdws'
import {getLanguage, t} from '../i18n'

// i18n 词条存在就用它，否则回退到内置文案（避免为两句话改字典，也避免键缺失时显示裸键名）
function tOr(key, fallback) {
	var v = t(key);
	return v === key ? fallback : v;
}

export default class ArtistList extends Component {

	state = {
		artists: [],
		loaded: false,
		error: null,
		search: "",
		uid1: UniqueID(),
		uid2: UniqueID(),
	}

	constructor(props, context) {
		super(props, context);
		this.search = this.search.bind(this);
		this.onSearchKeyDown = this.onSearchKeyDown.bind(this);
		this.onDocumentKeyDown = this.onDocumentKeyDown.bind(this);
		this.clearSearch = this.clearSearch.bind(this);
		this.focusSearch = this.focusSearch.bind(this);
		// 未配置 Subsonic 时不要发请求，避免 artists 区永远转圈
		if (this.subsonicConfigured()) this.loadArtists();
	}

	subsonicConfigured() {
		return !!(this.props.subsonic && this.props.subsonic.url);
	}

	componentDidMount() {
		$('#' + this.state.uid1).accordion({exclusive: false});//Artist
		$('#' + this.state.uid2).accordion({exclusive: false});//Library
		// "/" 聚焦搜索框（在输入控件内不拦截）；Esc 在输入框上清空并失焦
		document.addEventListener('keydown', this.onDocumentKeyDown);
	}

	componentWillUnmount() {
		document.removeEventListener('keydown', this.onDocumentKeyDown);
	}

	focusSearch() {
		if (this.searchInput) this.searchInput.focus();
	}

	clearSearch() {
		// 先同步清掉 DOM 值：失焦会同步触发 change，若此时输入框还是旧值，
		// onChange 会把旧值重新写回 state，导致“清不掉”。
		if (this.searchInput) this.searchInput.value = "";
		this.setState({search: ""});
	}

	// 输入框内按 Esc：清空关键词并失焦
	onSearchKeyDown(e) {
		if (e.key === 'Escape' || e.key === 'Esc') {
			e.preventDefault();
			this.clearSearch();
			if (this.searchInput) this.searchInput.blur();
		}
	}

	// 全局 "/" 快捷键：不在输入/多行/可编辑控件里时才聚焦搜索框
	onDocumentKeyDown(e) {
		if (e.key !== '/' || e.ctrlKey || e.metaKey || e.altKey) return;
		var el = e.target;
		var tag = el && el.tagName ? el.tagName.toLowerCase() : '';
		if (tag === 'input' || tag === 'textarea' || tag === 'select' || (el && el.isContentEditable)) return;
		e.preventDefault();
		this.focusSearch();
	}

	componentDidUpdate(prevProps, prevState) {
		if (prevProps.subsonic != this.props.subsonic) {
			if (this.subsonicConfigured()) {
				this.loadArtists();
			} else {
				// 配置被清空：清掉 artists 状态，别留下会转圈的 loader
				this.setState({artists: [], loaded: false, error: null});
			}
		}
	}

	loadArtists() {
		this.props.subsonic.getArtists({
			success: function(data) {
				this.setState({artists: data.artists, loaded: true, error: null});
			}.bind(this),
			error: function(err) {
				this.setState({error: <IconMessage type="negative" icon="warning circle" header="" message={t('browser.failedArtists')} />, loaded: true});
				console.error(this, err);
				Messages.message(this.props.events, t('browser.unableArtists', {error: err.message}), "error", "warning sign");
			}.bind(this)
		})
	}

	search(e) {
		this.setState({search: e.target.value});
	}

	render() {
		var subsonicConfigured = this.subsonicConfigured();

		// 本地曲库：搜索框过滤的就是它
		var library = <Library subsonic={this.props.subsonic} events={this.props.events} iconSize={this.props.iconSize} search={this.state.search} />;

		// 在线（Subsonic）艺术家区：
		// - 未配置（url 为空）时**整块不渲染**：不再出现"加载失败，请检查设置"占版面；
		// - 已配置时放进与本地曲库同款的可折叠块（默认收起），失败只在块内给一行淡色提示。
		var artistsBlock = null;
		if (subsonicConfigured) {
			var isZh = String(getLanguage() || '').toLowerCase().indexOf('zh') === 0;
			var artistsBody;

			if (this.state.error) {
				artistsBody = (
					<div className="subsonic-hint">
						<i className="plug icon"></i>
						{tOr('browser.artistsOffline', isZh ? '未连接 Subsonic，可在设置里填写' : 'Subsonic not connected \u2014 check Settings')}
					</div>
				);
			} else {
				var artistItems = this.state.artists
				.filter(function (artist) {
					return this.state.search == '' || artist.name.toLowerCase().indexOf(this.state.search.toLowerCase()) !== -1;
				}.bind(this))
				.map(function (artist) {
					return (
						<Artist key={artist.id} subsonic={this.props.subsonic} events={this.props.events} data={artist} iconSize={this.props.iconSize} />
					);
				}.bind(this));

				if (!this.state.loaded && artistItems.length == 0) {
					artistItems = <div className="ui inverted active centered inline loader"></div>;
				}

				artistsBody = (
					<div className="ui inverted tiny selection list">
						{artistItems}
					</div>
				);
			}

			artistsBlock = (
				<div className="ui inverted fluid accordion artistList" id={this.state.uid1}>
					<div className="title">
						<i className="dropdown icon"></i>
						{tOr('browser.artists', isZh ? '艺术家（在线）' : 'Artists (online)')}
					</div>
					<div className="ui secondary inverted segment content">
						{artistsBody}
					</div>
				</div>
			);
		}


		return (
			<div className="ui inverted basic segment">
				{/* 左侧面板顶栏：吸顶的搜索框（位于「本地曲库」折叠标题之上） */}
				<div className="library-search-bar">
					<div className="ui inverted transparent fluid left icon input">
						<i className="search icon"></i>
						<input ref={(el) => { this.searchInput = el; }} type="text" placeholder={subsonicConfigured ? t('browser.search') : t('browser.searchLibrary')} value={this.state.search} onInput={this.search} onChange={this.search} onKeyDown={this.onSearchKeyDown}/>
					</div>
					{this.state.search != ""
						? <button type="button" className="library-search-clear" title={t('browser.clearSearch')} aria-label={t('browser.clearSearch')} onClick={this.clearSearch}><i className="times icon"></i></button>
						: null}
				</div>
				<div className="ui inverted fluid accordion" id={this.state.uid2}>
					{library}
				</div>
				<div className="ui inverted divider"></div>
				{artistsBlock}
			</div>
		);
	}
}

export class Artist extends Component {
	state = {
		albums: [],
		loaded: false
	}

	constructor(props, context) {
		super(props, context);

		this.loadAlbums = this.loadAlbums.bind(this);
		this.onClick = this.onClick.bind(this);
	}

	loadAlbums() {
		this.props.subsonic.getArtist({
			id: this.props.data.id,
			success: function(data) {
				this.setState({albums: data.albums, loaded: true});
				//console.log("albums:"+JSON.stringify( data.albums));
			}.bind(this),
			error: function(err) {
				console.error(this, err);
				Messages.message(this.props.events, t('browser.unableArtistAlbums', {error: err.message}), "error", "warning sign");
			}.bind(this)
		});
	}

	onClick() {
		if (!this.state.loaded) {
			this.loadAlbums();
		}
	}

	render() {
		var albums = this.state.albums.map(function (album) {
			return (
				<Album key={album.id} subsonic={this.props.subsonic} events={this.props.events} data={album} iconSize={this.props.iconSize} />
			);
		}.bind(this));

		if (!this.state.loaded && albums.length == 0) {
			albums = <div className="ui inverted active centered inline loader"></div>
		}

		return (
			<div key={this.props.data.id} onClick={this.onClick}>
				<div className="title">
					<i className="dropdown icon"></i>
					{this.props.data.name} ({this.props.filter ? Object.keys(this.props.filter).length : this.props.data.albumCount})
				</div>
				<div className="ui secondary inverted segment content">
					<div className="ui inverted tiny selection list">
						{albums}
					</div>
				</div>
			</div>
		);
	}
}

class Album extends Component {

	constructor(props, context) {
		super(props, context);

		this.onClick = this.onClick.bind(this);
	}

	onClick() {
		this.props.subsonic.getAlbum({
			id: this.props.data.id,
			success: function(data) {
				this.props.events.publish({event: "browserSelected", data: {tracks: data.album}});
				console.log("albums:"+JSON.stringify( data.album));
			}.bind(this),
			error: function(err) {
				console.error(this, err);
				Messages.message(this.props.events, t('browser.unableAlbum', {error: err.message}), "error", "warning sign");
			}.bind(this)
		});
	}

	render() {
		var year = this.props.data.year ? '[' + this.props.data.year + ']' : '';
		return (
			<div className="item" onClick={this.onClick}>
				<CoverArt subsonic={this.props.subsonic} id={this.props.data.coverArt} size={this.props.iconSize} />
				<div className="content">
					<div className="header">{this.props.data.name}</div>
					<div className="description">{year} {t('browser.tracks', {count: this.props.data.songCount})}</div>
					<div className="extra">
					</div>
				</div>
			</div>
		);
	}
}

export class Library extends Component {
	state = {
		albums: [],
		loaded: false,
		error: null,
		selected: {},
		tracksLoading: false,
		tracksVersion: 0,
		// 行内展开的专辑 id 集合（accordion，可同时展开多个）
		expanded: {},
		// 正在拉取曲目详情的专辑 id（显示小 loader）
		expanding: {},
		// 当前播放曲目 id（来自 songchange），用于展开区高亮
		playingId: null
	}

	// 按专辑 id 缓存的完整专辑对象（含 song 数组），用于播放/入队/搜索曲目名
	albumCache = {};
	// 曲库全量曲目索引是否已在拉取/已完成（只做一次）
	trackDataRequested = false;

	constructor(props, context) {
		super(props, context);

		this.loadAlbums = this.loadAlbums.bind(this);
		this.onClick = this.onClick.bind(this);
		this.fetchAlbum = this.fetchAlbum.bind(this);
		this.toggleSelect = this.toggleSelect.bind(this);
		this.toggleAll = this.toggleAll.bind(this);
		this.addSelectedToQueue = this.addSelectedToQueue.bind(this);
		this.scanLibrary = this.scanLibrary.bind(this);
		this.playAlbum = this.playAlbum.bind(this);
		this.enqueueAlbum = this.enqueueAlbum.bind(this);
		this.viewAlbum = this.viewAlbum.bind(this);
		this.playTrack = this.playTrack.bind(this);
		this.enqueueTrack = this.enqueueTrack.bind(this);
		this.toggleExpand = this.toggleExpand.bind(this);
		this.ensureAlbumTracks = this.ensureAlbumTracks.bind(this);

		// 当前播放曲目变化：用于展开区高亮（曲目 id 与 queue 中的 id 同为文件 uri）
		props.events.subscribe({subscriber: this, event: ["songchange"]});
	}

	receive(event) {
		if (event.event == "songchange") {
			this.setState({playingId: event.data ? event.data.id : null});
		}
	}

	componentDidMount() {
		// 进页面就加载本地曲库，搜索与多选才有数据可用
		this.loadAlbums();
	}

	componentDidUpdate(prevProps) {
		// 输入即搜：有关键词时补齐曲目数据（懒加载，只做一次）
		if ((this.props.search || "").trim() != "" && prevProps.search != this.props.search) {
			this.ensureTrackData();
		}
	}

	loadAlbums() {
		fetch("/api/library")
		.then(response =>{
			if (!response.ok) {
				throw new Error(t('errors.httpFailed', {status: response.status}));
			}
			return response.json();//注意：返回的是JavaScript 对象
		})
		.then((data) => {
			// 有的后端会把 song[] 一起返回；有就直接当缓存用，省一次 POST
			(data || []).forEach((album) => {
				if (album && album.song) this.albumCache[album.id] = album;
			});
			this.trackDataRequested = false;
			this.setState({albums: data, loaded:true, error:null});
		})
		.catch(error => {
			console.error('request error:', error.message);
			this.setState({loaded: true, error: error.message});
			Messages.message(this.props.events, t('browser.unableLibrary', {error: error.message}), "error", "warning sign");
		});
	}

	onClick() {
		if (!this.state.loaded) {
			this.loadAlbums();
		}
	}

	/*
	* 行内展开/收起某张专辑的曲目列表（accordion，可同时展开多个）。
	* 首次展开时若专辑详情（含 song[]）还没缓存，再 POST /api/library 拉取。
	*/
	toggleExpand(id) {
		var expanded = Object.assign({}, this.state.expanded);
		expanded[id] = !expanded[id];
		this.setState({expanded: expanded});
		if (expanded[id]) this.ensureAlbumTracks(id);
	}

	ensureAlbumTracks(id) {
		var cached = this.albumCache[id];
		if (cached && cached.song) return; // 已有曲目，直接渲染
		if (this.state.expanding[id]) return; // 已在拉取

		var expanding = Object.assign({}, this.state.expanding);
		expanding[id] = true;
		this.setState({expanding: expanding});

		this.fetchAlbum(id)
		.then(() => {
			this.setState(function(prev) {
				var e = Object.assign({}, prev.expanding);
				delete e[id];
				return {expanding: e, tracksVersion: prev.tracksVersion + 1};
			});
		})
		.catch((error) => {
			console.error(error);
			this.setState(function(prev) {
				var e = Object.assign({}, prev.expanding);
				delete e[id];
				return {expanding: e};
			});
			Messages.message(this.props.events, t('browser.unableAlbum', {error: error.message}), "error", "warning sign");
		});
	}

	/*
	* 曲目级搜索的数据准备：把还没缓存的专辑详情（含 song[]）通过已有的
	* POST /api/library 拉回来，仅做一次；完成后触发一次重渲染。
	* 不新增后端接口。
	*/
	ensureTrackData() {
		if (this.trackDataRequested) return;
		this.trackDataRequested = true;

		var pending = (this.state.albums || []).filter(function(album) {
			var cached = this.albumCache[album.id];
			return !(cached && cached.song);
		}.bind(this));

		if (pending.length == 0) return;

		this.setState({tracksLoading: true});

		var self = this;
		var idx = 0;
		var concurrency = 6;
		var active = 0;

		function done() {
			active--;
			if (idx >= pending.length && active == 0) {
				self.setState({tracksLoading: false, tracksVersion: self.state.tracksVersion + 1});
			}
		}

		function next() {
			if (idx >= pending.length) return;
			var album = pending[idx++];
			active++;
			self.fetchAlbum(album.id)
			.then(function() { done(); next(); })
			.catch(function() { done(); next(); });
		}

		for (var i = 0; i < concurrency; i++) next();
	}

	/*
	* 曲目级匹配：在已缓存的专辑 song[] 里按 标题/艺术家/专辑名 找曲目。
	*/
	trackResults() {
		var search = (this.props.search || "").toLowerCase().trim();
		if (search == "") return [];

		var out = [];
		var seen = {};
		Object.keys(this.albumCache).forEach(function(id) {
			var album = this.albumCache[id];
			if (!album || !album.song) return;
			album.song.forEach(function(track) {
				if ((track.title || "").toLowerCase().indexOf(search) !== -1 ||
					(track.artist || "").toLowerCase().indexOf(search) !== -1 ||
					(track.album || "").toLowerCase().indexOf(search) !== -1) {
					var key = id + ':' + track.id;
					if (!seen[key]) { seen[key] = 1; out.push(track); }
				}
			});
		}.bind(this));
		return out;
	}

	// 单曲行 ▶：替换队列并播放这一首
	playTrack(song) {
		this.props.events.publish({event: "playerEnqueue", data: {action: "REPLACE", tracks: [song]}});
	}

	// 单曲行 ＋：加入队列
	enqueueTrack(song) {
		this.props.events.publish({event: "playerEnqueue", data: {action: "ADD", tracks: [song]}});
	}

	/**
	* 取某专辑的完整对象（含 song 数组）。POST /api/library 的返回对象带 song。
	*/
	fetchAlbum(id) {
		if (this.albumCache[id]) return Promise.resolve(this.albumCache[id]);

		return fetch("/api/library", {
			method: 'POST',
			headers: {
				'Content-Type': 'application/json',
				'Accept': 'application/json',
			},
			body: JSON.stringify({album: id})
		})
		.then(response =>{
			if (!response.ok) {
				throw new Error(t('errors.httpFailed', {status: response.status}));
			}
			return response.json();
		})
		.then((data) => {
			this.albumCache[id] = data;
			return data;
		});
	}

	selectedIds() {
		return Object.keys(this.state.selected).filter(function(id) {
			return this.state.selected[id];
		}.bind(this));
	}

	toggleSelect(id, checked) {
		var selected = Object.assign({}, this.state.selected);
		selected[id] = checked;
		this.setState({selected: selected});
	}

	// 全选/全不选：作用于当前搜索过滤后的专辑
	toggleAll(checked) {
		var selected = {};
		this.filteredAlbums().forEach(function(album) {
			selected[album.id] = checked;
		});
		this.setState({selected: selected});
	}

	allSelected() {
		var albums = this.filteredAlbums();
		return albums.length > 0 && albums.every(function(album) {
			return !!this.state.selected[album.id];
		}.bind(this));
	}

	// "Play"/"Queue" 按钮共用：取回专辑曲目后交给播放器已有的 REPLACE/ADD 分支
	queueAlbum(id, action, errorKey) {
		this.fetchAlbum(id)
		.then(album => {
			if (!album.song || album.song.length == 0) {
				Messages.message(this.props.events, t('browser.noTracks'), "warning", "warning sign");
				return;
			}
			this.props.events.publish({event: "playerEnqueue", data: {action: action, tracks: album.song}});
		})
		.catch(error => {
			console.error(error);
			Messages.message(this.props.events, t(errorKey, {error: error.message}), "error", "warning sign");
		});
	}

	playAlbum(id) {
		this.queueAlbum(id, "REPLACE", 'browser.unablePlayAlbum');
	}

	enqueueAlbum(id) {
		this.queueAlbum(id, "ADD", 'browser.unableAddAlbum');
	}

	// 行点击：把专辑（含曲目）送给 Selection 标签页
	viewAlbum(id) {
		this.fetchAlbum(id)
		.then(album => {
			this.props.events.publish({event: "browserSelected", data: {tracks: album}});
		})
		.catch(error => {
			console.error(error);
			Messages.message(this.props.events, t('browser.unableAlbum', {error: error.message}), "error", "warning sign");
		});
	}

	// 多选/全选：汇总所有选中专辑的曲目，一次 POST 整个数组
	addSelectedToQueue() {
		var ids = this.selectedIds();
		if (ids.length == 0) {
			Messages.message(this.props.events, t('browser.selectFirst'), "warning", "warning sign");
			return;
		}

		Promise.all(ids.map(id => this.fetchAlbum(id)))
		.then(albums => {
			var tracks = [];
			albums.forEach(function(album) {
				if (album.song) tracks = tracks.concat(album.song);
			});
			if (tracks.length == 0) {
				Messages.message(this.props.events, t('browser.selectedNoTracks'), "warning", "warning sign");
				return;
			}
			this.props.events.publish({event: "playerEnqueue", data: {action: "ADD", tracks: tracks}});
			this.setState({selected: {}});
		})
		.catch(error => {
			console.error(error);
			Messages.message(this.props.events, t('browser.unableAddSelection', {error: error.message}), "error", "warning sign");
		});
	}

	// 触发 mpd 全量曲库扫描（MPD_API_UPDATE_DB）。换过 mpd.conf / music_directory 后用它。
	scanLibrary(e) {
		if (e && e.stopPropagation) e.stopPropagation();
		sendCommand("MPD_API_UPDATE_DB");
		Messages.message(this.props.events, t('browser.scanRequested'), "info", "info");
	}

	// 搜索源 = 本地曲库专辑（名称/艺术家/年份，已取回的曲目名也参与匹配）
	filteredAlbums() {
		var search = (this.props.search || "").toLowerCase();
		if (search == "") return this.state.albums;

		return this.state.albums.filter(function(album) {
			if ((album.name || "").toLowerCase().indexOf(search) !== -1) return true;
			if ((album.artist || "").toLowerCase().indexOf(search) !== -1) return true;
			if (String(album.year || "").indexOf(search) !== -1) return true;

			var cached = this.albumCache[album.id];
			if (cached && cached.song) {
				return cached.song.some(function(track) {
					return (track.title || "").toLowerCase().indexOf(search) !== -1;
				});
			}
			return false;
		}.bind(this));
	}

	render() {
		var search = (this.props.search || "").trim();

		var albums = this.filteredAlbums().map(function (album) {
			var cached = this.albumCache[album.id];
			return (
				<LibraryAlbum key={album.id} subsonic={this.props.subsonic} events={this.props.events} data={album} iconSize={this.props.iconSize}
					selected={!!this.state.selected[album.id]} onToggleSelect={this.toggleSelect}
					onPlay={this.playAlbum} onEnqueue={this.enqueueAlbum} onView={this.viewAlbum}
					expanded={!!this.state.expanded[album.id]} expanding={!!this.state.expanding[album.id]}
					tracks={cached && cached.song ? cached.song : null} playingId={this.state.playingId}
					onToggleExpand={this.toggleExpand} onPlayTrack={this.playTrack} onEnqueueTrack={this.enqueueTrack} />
			);
		}.bind(this));

		if (!this.state.loaded && albums.length == 0) {
			albums = <div className="ui inverted active centered inline loader"></div>
		}

		// 关键词搜索：曲目级结果（▶ 播放 / ＋ 入队）与匹配的专辑分组同时展示
		var tracks = search == "" ? [] : this.trackResults();
		var trackSection = null;
		if (search != "") {
			if (tracks.length > 0) {
				trackSection = (
					<div className="library-tracks">
						<div className="library-tracks-title">{t('browser.matchingTracks', {count: tracks.length})}</div>
						<div className="ui inverted tiny selection list">
							{tracks.map(function(song) {
								return <LibraryTrack key={song.id} song={song} onPlay={this.playTrack} onEnqueue={this.enqueueTrack} />;
							}.bind(this))}
						</div>
					</div>
				);
			} else if (this.state.tracksLoading) {
				trackSection = <div className="library-search-status">{t('browser.searchingTracks')}</div>;
			} else if (this.state.loaded && albums.length == 0) {
				trackSection = <div className="library-no-results">{t('browser.noResults', {query: search})}</div>;
			}
		}

		return (
			<div onClick={this.onClick}>
				<div className="title">
					<i className="dropdown icon"></i>
					{t('browser.library')}
				</div>
				<div className="ui secondary inverted segment content">
					<div className="library-controls">
						<label className="library-select-all">
							<span className="library-checkbox">
								<input type="checkbox" checked={this.allSelected()} onChange={(e) => this.toggleAll(e.target.checked)} />
							</span>
							<span className="library-select-all-label">{t('browser.selectAll')}</span>
						</label>
						<button className="ui mini compact labelled icon olive button" title={t('browser.addSelectedTitle')} onClick={this.addSelectedToQueue}>
							<i className="plus icon"></i>
							{t('browser.addSelected', {count: this.selectedIds().length})}
						</button>
						<button className="ui mini compact labelled icon teal button" title={t('browser.scanTitle')} onClick={this.scanLibrary}>
							<i className="sync icon"></i>
							{t('browser.scanLibrary')}
						</button>
					</div>
					{trackSection}
					<div className="ui inverted tiny selection list">
						{albums}
					</div>
				</div>
			</div>
		);
	}
}

/**
* 搜索结果里的单曲行：▶ 播放这首，＋ 加入队列。
*/
class LibraryTrack extends Component {

	onPlay(e) {
		e.stopPropagation();
		this.props.onPlay(this.props.song);
	}

	onEnqueue(e) {
		e.stopPropagation();
		this.props.onEnqueue(this.props.song);
	}

	render() {
		var song = this.props.song;
		var meta = [song.artist, song.album].filter(function(v) { return v; }).join(' - ');
		return (
			<div className={"item library-track" + (this.props.playing ? " playing" : "")}>
				<div className="library-track-actions">
					<button className="ui mini compact icon green button" title={t('browser.playTrack')} onClick={this.onPlay.bind(this)}>
						<i className="play icon"></i>
					</button>
					<button className="ui mini compact icon olive button" title={t('browser.queueTrack')} onClick={this.onEnqueue.bind(this)}>
						<i className="plus icon"></i>
					</button>
				</div>
				<div className="content">
					<div className="header">{song.title}</div>
					<div className="description">{meta}</div>
				</div>
			</div>
		);
	}
}

class LibraryAlbum extends Component {

	state = {
		// 收起后延迟卸载曲目，让 max-height 过渡能跑完
		rendered: false
	}

	constructor(props, context) {
		super(props, context);

		this.onRowClick = this.onRowClick.bind(this);
		this.onRowKeyDown = this.onRowKeyDown.bind(this);
		this.onCheck = this.onCheck.bind(this);
		this.onPlay = this.onPlay.bind(this);
		this.onEnqueue = this.onEnqueue.bind(this);
		this.onOpenSelection = this.onOpenSelection.bind(this);
		this.onToggleArrow = this.onToggleArrow.bind(this);
	}

	componentDidMount() {
		if (this.props.expanded) this.setState({rendered: true});
	}

	componentDidUpdate(prevProps) {
		if (this.props.expanded && !this.state.rendered) {
			clearTimeout(this._unmountTimer);
			this.setState({rendered: true});
		} else if (!this.props.expanded && prevProps.expanded && this.state.rendered) {
			clearTimeout(this._unmountTimer);
			this._unmountTimer = setTimeout(() => this.setState({rendered: false}), 300);
		}
	}

	componentWillUnmount() {
		clearTimeout(this._unmountTimer);
	}

	// 点击专辑行 → 就地展开/收起曲目（不再强制跳转 Selection）
	onRowClick() {
		this.props.onToggleExpand(this.props.data.id);
	}

	// 键盘可操作：焦点在标题上时回车/空格切换
	onRowKeyDown(e) {
		if (e.target !== e.currentTarget) return;
		if (e.key === 'Enter' || e.key === ' ' || e.key === 'Spacebar') {
			e.preventDefault();
			this.props.onToggleExpand(this.props.data.id);
		}
	}

	onCheck(e) {
		e.stopPropagation();
		this.props.onToggleSelect(this.props.data.id, e.target.checked);
	}

	onPlay(e) {
		e.stopPropagation();
		this.props.onPlay(this.props.data.id);
	}

	onEnqueue(e) {
		e.stopPropagation();
		this.props.onEnqueue(this.props.data.id);
	}

	// 次要入口：仍然可以在「选中」标签页打开这张专辑
	onOpenSelection(e) {
		e.stopPropagation();
		this.props.onView(this.props.data.id);
	}

	onToggleArrow(e) {
		e.stopPropagation();
		this.props.onToggleExpand(this.props.data.id);
	}

	tracksBody() {
		var tracks = this.props.tracks;
		if (tracks && tracks.length > 0) {
			return (
				<div className="ui inverted tiny selection list library-album-track-list">
					{tracks.map(function(song) {
						return (
							<LibraryTrack key={song.id} song={song} playing={song.id === this.props.playingId}
								onPlay={this.props.onPlayTrack} onEnqueue={this.props.onEnqueueTrack} />
						);
					}.bind(this))}
				</div>
			);
		}
		if (this.props.expanding || !tracks) {
			return <div className="library-album-tracks-status"><div className="ui inverted active centered inline loader"></div></div>;
		}
		return <div className="library-album-tracks-status">{t('browser.noTracks')}</div>;
	}

	render() {
		var year = this.props.data.year ? '[' + this.props.data.year + ']' : '';
		var expanded = this.props.expanded;
		var toggleTitle = expanded ? t('browser.collapseTracks') : t('browser.expandTracks');
		return (
			<div className={"item library-album" + (expanded ? " expanded" : "")}>
				<div className="library-album-row" onClick={this.onRowClick}>
					<div className="library-checkbox" onClick={(e) => e.stopPropagation()}>
						<input type="checkbox" checked={this.props.selected} onChange={this.onCheck} />
					</div>
					<CoverArt subsonic={this.props.subsonic} id={this.props.data.coverart} size={this.props.iconSize} />
					<div className="content">
						<div className="header" role="button" tabIndex="0" aria-expanded={expanded ? "true" : "false"}
							title={toggleTitle} onKeyDown={this.onRowKeyDown}>{this.props.data.name}</div>
						<div className="description">{year} {t('browser.tracks', {count: this.props.data.songCount})}</div>
						<div className="extra">
							<button className="ui mini compact labelled icon green button" title={t('browser.playAll')} onClick={this.onPlay}>
								<i className="play icon"></i>
								{t('browser.play')}
							</button>
							<button className="ui mini compact labelled icon olive button" title={t('browser.addAllToQueue')} onClick={this.onEnqueue}>
								<i className="plus icon"></i>
								{t('browser.queue')}
							</button>
						</div>
					</div>
					<div className="library-album-actions">
						<button type="button" className="ui mini compact icon basic button library-open-selection"
							title={t('browser.openInSelection')} aria-label={t('browser.openInSelection')} onClick={this.onOpenSelection}>
							<i className="external alternate icon"></i>
						</button>
						<button type="button" className="ui mini compact icon basic button library-expand-toggle"
							title={toggleTitle} aria-label={toggleTitle} aria-expanded={expanded ? "true" : "false"} onClick={this.onToggleArrow}>
							<i className={(expanded ? "chevron down" : "chevron right") + " icon"}></i>
						</button>
					</div>
				</div>
				<div className={"library-album-tracks" + (expanded ? " open" : "")} aria-hidden={expanded ? "false" : "true"}>
					<div className="library-album-tracks-inner" onClick={(e) => e.stopPropagation()}>
						{this.state.rendered ? this.tracksBody() : null}
					</div>
				</div>
			</div>
		);
	}
}
