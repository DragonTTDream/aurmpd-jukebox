import {h, Component} from 'preact';
import moment from 'moment'
import {IconMessage,CoverArt,Prompt,InputPrompt,ListPrompt} from './common'
import TrackList from './tracklist'
import {sendCommand} from '../mpdws'
import {SecondsToTime,UniqueID} from '../util'
import {Messages} from './app'
import {t} from '../i18n'

export default class PlaylistManager extends Component {

	state = {
		playlists: [],
		playlist: null
	}

	constructor(props, context) {
		super(props, context);

		this.loadPlaylists = this.loadPlaylists.bind(this);
		this.loadPlaylist = this.loadPlaylist.bind(this);
		this.createPlaylist = this.createPlaylist.bind(this);
		this.updatePlaylist = this.updatePlaylist.bind(this);
		this.receive = this.receive.bind(this);

		if (this.subsonicConfigured()) this.loadPlaylists();

		props.events.subscribe({
			subscriber: this,
			event: ["playlistManage"]
		});
	}

	// 只有真正配置过 Subsonic(url 存在且 localStorage 里有 url)时才走 Subsonic 歌单；
	// index.js 会给 subsonic.url 兜底一个 demo 地址，所以仅看 url 不够。
	subsonicConfigured() {
		return !!(this.props.subsonic && this.props.subsonic.url && localStorage.getItem('url'));
	}


	componentDidUpdate(prevProps, prevState) {
		if (prevProps.subsonic !== this.props.subsonic && this.subsonicConfigured()) this.loadPlaylists();
	}

	receive(event) {
		if (event.event === "playlistManage") {
			if (!this.subsonicConfigured()) {
				// 本地(mpd)歌单：Queue 标签页的 "Add to Playlist" 就是"保存当前队列为歌单"
				if ((event.data.action === "ADD" || event.data.action === "CREATE") && this.localManager) {
					this.localManager.saveQueue();
				} else {
					Messages.message(this.props.events, t('playlist.useLocal'), "info", "list");
				}
				return;
			}

			if (event.data.action === "ADD") {
				this.lister.show(function(approved, playlist) {
					if (!approved) return;

					var tracks = event.data.tracks.map(t => t.id);

					var currentPlaylist = this.state.playlists.find(p => p.id === playlist)

					if (currentPlaylist === undefined) {
						this.createPlaylist(playlist, tracks);
					} else {
						this.updatePlaylist(playlist, tracks, []);
					}
				}.bind(this));
			} else if (event.data.action === "CREATE") {
				this.creator.show("", function(approved, newName) {
					if (!approved) return;

					this.createPlaylist(newName, []);
				}.bind(this));
			} else if (event.data.action === "DELETE") {
				this.deleter.show(function(approved) {
					if (!approved) return;

					this.props.subsonic.deletePlaylist({
						id: event.data.id,
						success: function() {
							this.loadPlaylists();
							Messages.message(this.props.events, t('playlist.deleted'), "warning", "trash");
						}.bind(this)
					});
				}.bind(this));
			} else if (event.data.action === "RENAME") {
				this.renamer.show(event.data.name, function(approved, newName) {
					if (!approved) return;

					this.props.subsonic.updatePlaylist({
						id: event.data.id,
						name: newName,
						success: function() {
							this.loadPlaylists();
							Messages.message(this.props.events, t('playlist.renamed'), "success", "edit");
						}.bind(this)
					});
				}.bind(this));
			} else if (event.data.action === "REMOVE") {
				// load up the playlist, since we can only remove tracks by their index within a playlist
				this.props.subsonic.getPlaylist({
					id: event.data.id,
					success: function(data) {
						var tracks = event.data.tracks.map(function(t) {
							for (var i = 0; i < data.playlist.entry.length; i++) {
								if (t.id === data.playlist.entry[i].id) return i;
							}
						});

						this.updatePlaylist(event.data.id, [], tracks);
					}.bind(this),
					error: function(err) {
						console.error(this, err);
						Messages.message(this.props.events, t('playlist.unableLoad', {error: err.message}), "error", "warning sign");
					}.bind(this)
				});
			}
		}
	}

	createPlaylist(name, trackIds) {
		this.props.subsonic.createPlaylist({
			name: name,
			tracks: trackIds,
			success: function() {
				Messages.message(this.props.events, t('playlist.created', {name: name}), "success", "checkmark");
				this.loadPlaylists();
			}.bind(this),
			error: function(err) {
				console.error(this, err);
				Messages.message(this.props.events, t('playlist.createFailed', {error: err.message}), "error", "warning sign");
			}.bind(this)
		});
	}

	updatePlaylist(id, add, remove) {
		this.props.subsonic.updatePlaylist({
			id: id,
			add: add,
			remove: remove,
			success: function() {
				Messages.message(this.props.events, t('playlist.updated'), "success", "checkmark");
				this.loadPlaylists();
				if (this.state.playlist !== null && id === this.state.playlist.id) this.loadPlaylist(id);
			}.bind(this),
			error: function(err) {
				console.error(this, err);
				Messages.message(this.props.events, t('playlist.updateFailed', {error: err.message}), "error", "warning sign");
			}.bind(this)
		});
	}

	loadPlaylists() {
		this.props.subsonic.getPlaylists({
			success: function(data) {
				this.setState({playlists: data.playlists});
				if (this.state.playlist != null) {
					this.loadPlaylist(this.state.playlist.id);
				}
			}.bind(this),
			error: function(err) {
				console.error(this, err);
				Messages.message(this.props.events, t('playlist.unableGet', {error: err.message}), "error", "warning sign");
			}.bind(this)
		});
	}

	loadPlaylist(id) {
		this.props.subsonic.getPlaylist({
			id: id,
			success: function(data) {
				this.setState({playlist: data.playlist});
			}.bind(this),
			error: function(err) {
				console.error(this, err);
				Messages.message(this.props.events, t('playlist.unableLoad', {error: err.message}), "error", "warning sign");
			}.bind(this)
		});
	}

	render() {
		// 未配置 Subsonic 时走本地 mpd 歌单(局域网点歌机场景)，配置了则保持原有 Subsonic 行为不变
		if (!this.subsonicConfigured()) {
			return <LocalPlaylistManager ref={(r) => {this.localManager = r;}} events={this.props.events} iconSize={this.props.iconSize} />;
		}

		var playlists = [];
		if (this.state.playlists) {
			playlists = this.state.playlists.map(function (playlist) {
				return (
					<PlaylistSelectorItem key={playlist.id} subsonic={this.props.subsonic} data={playlist} iconSize={this.props.iconSize} simple={true} />
				);
			}.bind(this));
		}

		return (
			<div className="playlistManager">
				<InputPrompt ref={(r) => {this.creator = r;}} title={t('playlist.createTitle')} message={t('playlist.enterNameMessage')} />
				<InputPrompt ref={(r) => {this.renamer = r;}} title={t('playlist.renameTitle')} message={t('playlist.enterNewNameMessage')} />
				<Prompt ref={(r) => {this.deleter = r;}} title={t('playlist.deleteTitle')} message={t('playlist.deleteMessage')} ok={t('settings.yes')} icon="red trash" />
				<ListPrompt ref={(r) => {this.lister = r;}} title={t('playlist.addTitle')} message={t('playlist.chooseTitle')} ok={t('playlist.add')} icon="teal list"
					defaultText={t('playlist.playlistsPlaceholder')} allowNew={true} items={playlists} />

				<PlaylistSelector subsonic={this.props.subsonic} events={this.props.events} iconSize={this.props.iconSize} playlists={this.state.playlists} selected={this.loadPlaylist} />
				<Playlist subsonic={this.props.subsonic} events={this.props.events} iconSize={this.props.iconSize} playlist={this.state.playlist} changed={this.loadPlaylists} />
			</div>
		);
	}
}

class PlaylistSelector extends Component {

	defaultProps = {
		playlists: []
	}

	constructor(props, context) {
		super(props, context);

		this.value = null;

		this.create = this.create.bind(this);
	}

	componentDidMount() {
		$('.playlistSelector .dropdown').dropdown({
			action: 'activate',
			onChange: function(value, text, selectedItem) {
				if (this.value !== value) {
					if (this.props.selected) this.props.selected(value);
					this.value = value;
				}
			}.bind(this)
		});
	}

	componentDidUpdate(prevProps, prevState) {
		if (this.value) $('.playlistSelector .dropdown').dropdown('set selected', this.value);
	}

	create() {
		this.props.events.publish({event: "playlistManage", data: {action: "CREATE"}});
	}

	render() {
		var playlists = [];
		if (this.props.playlists) {
			playlists = this.props.playlists.map(function (playlist) {
				return (
					<PlaylistSelectorItem key={playlist.id} subsonic={this.props.subsonic} data={playlist} iconSize={this.props.iconSize} />
				);
			}.bind(this));
		}

		return (
			<div className="ui basic segment playlistSelector">
				<div className="ui grid">
					<div className="thirteen wide column">
						<div className="ui fluid selection dropdown">
							<i className="dropdown icon"></i>
							<div className="default text">{t('playlist.playlistsPlaceholder')}</div>
							<div className="menu">
								{playlists}
							</div>
						</div>
					</div>
					<div className="three wide column">
						<button className="ui fluid labelled icon teal button" onClick={this.create}><i className="plus icon"></i> {t('playlist.newPlaylist')}</button>
					</div>
				</div>
			</div>
		);
	}
}

class PlaylistSelectorItem extends Component {
	render() {
		var description = !this.props.simple
		? <span className="description">{this.props.data.songCount} tracks, {SecondsToTime(this.props.data.duration)}</span>
		: null;

		return (
			<div className="item" data-value={this.props.data.id}>
				<CoverArt subsonic={this.props.subsonic} id={this.props.data.coverArt} size={this.props.iconSize} />
				{description}
				<span className="text">{this.props.data.name}</span>
			</div>
		);
	}
}

class Playlist extends Component {

	defaultProps = {
		playlist: null
	}

	constructor(props, context) {
		super(props, context);
	}

	render() {
		if (!this.props.playlist) {
			return (
				<div className="playlistView">
					<IconMessage icon="info circle" header={t('playlist.nothingHeader')} message={t('playlist.nothingMessage')} />
				</div>
			);
		} else {
			return (
				<div className="ui basic segment playlistView">
					<PlaylistInfo events={this.props.events} subsonic={this.props.subsonic} playlist={this.props.playlist} changed={this.props.changed} />
					<TrackList subsonic={this.props.subsonic} tracks={this.props.playlist.entry} events={this.props.events}
						playlist={this.props.playlist.id} iconSize={this.props.iconSize} />
				</div>
			);
		}
	}
}

class PlaylistInfo extends Component {

	constructor(props, context) {
		super(props, context);

		this.play = this.play.bind(this);
		this.enqueue = this.enqueue.bind(this);
		this.delete = this.delete.bind(this);
		this.rename = this.rename.bind(this);
	}

	play() {
		this.props.events.publish({event: "playerEnqueue", data: {action: "REPLACE", tracks: this.props.playlist.entry}});
	}

	enqueue() {
		this.props.events.publish({event: "playerEnqueue", data: {action: "ADD", tracks: this.props.playlist.entry}});
	}

	delete() {
		this.props.events.publish({event: "playlistManage", data: {action: "DELETE", id: this.props.playlist.id}});
	}

	rename() {
		this.props.events.publish({event: "playlistManage", data: {action: "RENAME", id: this.props.playlist.id, name: this.props.playlist.name}});
	}

	render() {
		return (
			<div className="ui items">
				<div className="item">
					<div className="ui small image">
						<CoverArt subsonic={this.props.subsonic} id={this.props.playlist.coverArt} size={200} events={this.props.events} />
					</div>
					<div className="aligned content">
						<div className="header">
							<div>{this.props.playlist.name}</div>
						</div>
						<div className="meta">
							<div>{t('selection.added', {date: moment(this.props.playlist.created).format("ll")})}</div>
							<div>{t('selection.updated', {date: moment(this.props.playlist.changed).format("ll")})}</div>
							<div>{t('selection.tracks', {count: this.props.playlist.songCount, duration: SecondsToTime(this.props.playlist.duration)})}</div>
						</div>
						<div className="extra">
							<button className="ui small compact labelled icon green button" onClick={this.play}><i className="play icon"></i> {t('playlist.play')}</button>
							<button className="ui small compact labelled icon olive button" onClick={this.enqueue}><i className="plus icon"></i> {t('playlist.addToQueue')}</button>
							<button className="ui small compact labelled icon grey button" onClick={this.rename}><i className="edit icon"></i> {t('playlist.rename')}</button>
							<button className="ui small compact labelled icon red button" onClick={this.delete}><i className="trash icon"></i> {t('playlist.delete')}</button>
						</div>
					</div>
				</div>
			</div>
		);
	}
}

/**
* 本地(mpd)歌单管理：未配置 Subsonic 时使用，命令经 /ws 发给后端。
* 订阅事件总线上的 mpdMessage(由 player.js 的 WS message 监听器广播)。
*/
export class LocalPlaylistManager extends Component {

	state = {
		playlists: [],
		playlist: null,
		selected: null
	}

	constructor(props, context) {
		super(props, context);

		this.refresh = this.refresh.bind(this);
		this.selectPlaylist = this.selectPlaylist.bind(this);
		this.saveQueue = this.saveQueue.bind(this);
		this.loadToQueue = this.loadToQueue.bind(this);
		this.deletePlaylist = this.deletePlaylist.bind(this);
		this.receive = this.receive.bind(this);

		props.events.subscribe({
			subscriber: this,
			event: ["mpdMessage"]
		});

		this.refresh();
	}

	componentWillUnmount() {
		this.props.events.unsubscribe({subscriber: this, event: ["mpdMessage"]});
	}

	receive(event) {
		if (event.event !== "mpdMessage") return;

		var response = event.data;
		if (response == null) return;

		if (response.type === "playlists") {
			var playlists = response.data || [];
			var selected = this.state.selected;
			// 选中的歌单已被删除/重命名时清空选择，避免后续命令作用于不存在的歌单
			if (selected != null && !playlists.some(function(p) { return p.name === selected; })) {
				this.setState({playlists: playlists, selected: null, playlist: null});
			} else {
				this.setState({playlists: playlists});
			}
		} else if (response.type === "playlist") {
			this.setState({playlist: response.data || null});
		} else if (response.type === "error") {
			Messages.message(this.props.events, t('playlist.mpdError', {error: response.data || t('playlist.unknownError')}), "error", "warning sign");
		}
	}

	refresh() {
		sendCommand("MPD_API_GET_PLAYLISTS");
	}

	selectPlaylist(name) {
		this.setState({selected: name});
		sendCommand("MPD_API_GET_PLAYLIST_SONGS," + name);
	}

	saveQueue() {
		this.saver.show("", function(approved, name) {
			if (!approved) return;

			name = (name || "").trim();
			if (!name) {
				Messages.message(this.props.events, t('playlist.enterName'), "error", "warning sign");
				return;
			}

			sendCommand("MPD_API_SAVE_QUEUE," + name);
			Messages.message(this.props.events, t('playlist.saving', {name: name}), "info", "save");
		}.bind(this));
	}

	loadToQueue() {
		var name = this.state.selected;
		if (!name) return;

		sendCommand("MPD_API_ADD_PLAYLIST," + name);
		Messages.message(this.props.events, t('playlist.loading', {name: name}), "info", "play");
	}

	deletePlaylist() {
		var name = this.state.selected;
		if (!name) return;

		this.deleter.show(function(approved) {
			if (!approved) return;

			sendCommand("MPD_API_RM_PLAYLIST," + name);
		}.bind(this));
	}

	render() {
		var selected = this.state.selected;
		var playlist = this.state.playlist;
		var tracks = playlist != null ? (playlist.song || []) : null;

		var content = null;
		if (tracks == null) {
			content = <IconMessage icon="info circle" header={t('playlist.nothingHeader')} message={t('playlist.nothingMessage')} />;
		} else if (tracks.length === 0) {
			content = <IconMessage icon="music" header={t('playlist.emptyHeader')} message={t('playlist.emptyMessage')} />;
		} else {
			var rows = tracks.map(function(song, index) {
				return (
					<tr key={song.uri || index}>
						<td>{index + 1}</td>
						<td>{song.title || song.uri}</td>
						<td>{song.artist || "-"}</td>
						<td>{song.album || "-"}</td>
						<td>{song.duration ? SecondsToTime(song.duration) : "-"}</td>
					</tr>
				);
			});

			content = (
				<table className="ui very basic compact table">
					<thead>
						<tr><th>{t('tracklist.number')}</th><th>{t('tracklist.title')}</th><th>{t('tracklist.artist')}</th><th>{t('tracklist.album')}</th><th>{t('tracklist.time')}</th></tr>
					</thead>
					<tbody>{rows}</tbody>
				</table>
			);
		}

		return (
			<div className="localPlaylistManager">
				<InputPrompt ref={(r) => {this.saver = r;}} title={t('playlist.saveQueueTitle')} message={t('playlist.enterNameMessage')} ok={t('playlist.save')} icon="teal save" />
				<Prompt ref={(r) => {this.deleter = r;}} title={t('playlist.deleteTitle')} message={t('playlist.deleteMessage')} ok={t('settings.yes')} icon="red trash" />

				<div className="ui basic segment">
					<div className="ui grid">
						<div className="thirteen wide column">
							<LocalPlaylistSelector playlists={this.state.playlists} selected={this.selectPlaylist} value={selected} />
						</div>
						<div className="three wide column">
							<button className="ui fluid labelled icon button" onClick={this.refresh}><i className="refresh icon"></i> {t('playlist.refresh')}</button>
						</div>
					</div>
					<button className="ui small compact labelled icon teal button" onClick={this.saveQueue}><i className="save icon"></i> {t('playlist.saveQueue')}</button>
					<button className="ui small compact labelled icon green button" disabled={!selected} onClick={this.loadToQueue}><i className="play icon"></i> {t('playlist.loadToQueue')}</button>
					<button className="ui small compact labelled icon red button" disabled={!selected} onClick={this.deletePlaylist}><i className="trash icon"></i> {t('playlist.delete')}</button>
				</div>

				<div className="ui basic segment playlistView">
					<div className="header">{playlist != null ? playlist.name : ""}</div>
					{content}
				</div>
			</div>
		);
	}
}

class LocalPlaylistSelector extends Component {

	static defaultProps = {
		playlists: [],
		value: null
	}

	constructor(props, context) {
		super(props, context);

		this.value = null;

		this.select = this.select.bind(this);
	}

	componentDidMount() {
		$('.localPlaylistSelector .dropdown').dropdown({
			action: 'activate',
			onChange: function(value, text, selectedItem) {
				if (this.value !== value) {
					if (this.props.selected) this.props.selected(value);
					this.value = value;
				}
			}.bind(this)
		});

		if (this.props.value) {
			this.value = this.props.value;
			$('.localPlaylistSelector .dropdown').dropdown('set selected', this.props.value);
		}
	}

	componentDidUpdate(prevProps, prevState) {
		if (this.props.value !== prevProps.value) {
			this.value = this.props.value;
			if (!this.props.value) $('.localPlaylistSelector .dropdown').dropdown('clear');
		}
		if (this.value) $('.localPlaylistSelector .dropdown').dropdown('set selected', this.value);
	}

	select(name) {
		if (this.props.selected) this.props.selected(name);
	}

	render() {
		var playlists = this.props.playlists || [];
		var items = playlists.map(function(playlist) {
			return (
				<div className="item" data-value={playlist.name} key={playlist.name}>
					<span className="text">{playlist.name}</span>
				</div>
			);
		});

		return (
			<div className="ui basic segment localPlaylistSelector">
				<div className="ui fluid selection dropdown">
					<i className="dropdown icon"></i>
					<div className="default text">{t('playlist.playlistsPlaceholder')}</div>
					<div className="menu">
						{items}
					</div>
				</div>
			</div>
		);
	}
}
