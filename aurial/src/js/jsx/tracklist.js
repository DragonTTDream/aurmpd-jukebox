import { h, Component } from 'preact';
import {IconMessage,CoverArt} from './common'
import {SecondsToTime} from '../util'
import {t} from '../i18n'

export default class TrackList extends Component {

	constructor(props, context) {
		super(props, context);
		this.state = {
			queue: props.tracks,
			currentsongid: -1
		}
		props.events.subscribe({
			subscriber: this,
			event: ["mpdstatus"]
		});
	}
	//当queue组件更新queue时，本地组件也同时更新queue
    componentDidUpdate(prevProps) {
        if (prevProps.tracks!== this.props.tracks) {
			this.setState({queue:this.props.tracks});
		}
	}	

	receive(event) {
		switch (event.event) {
			case "mpdstatus":
				if(event.data.currentsongid != this.state.currentsongid){//status的songid发生改变发送songchange
					var track =null;
					for (const find_track of this.state.queue) {
						if (find_track.queue_sid === event.data.currentsongid) {
							track = find_track;
							break;
						}
					}
					if(track != null){//需在queue里找到新的sid才做变更
						this.props.events.publish({event: "songchange",data:track});
						this.setState({currentsongid:event.data.currentsongid});
					}
				}
			break;
		}
	}

	render() {
		var tracks = []
		if (this.props.tracks && this.props.tracks.length > 0) {
			//console.log("this.props.tracks:"+JSON.stringify(this.props.tracks));
			tracks = this.props.tracks.map(function (entry) {
				return (
					<Track key={entry.id} subsonic={this.props.subsonic} events={this.props.events} track={entry}
						playing={this.state.currentsongid == entry.queue_sid}
						queued={'queue_sid' in entry} playlist={this.props.playlist}
						iconSize={this.props.iconSize} />
				);
			}.bind(this));
		}

		return (
			<div className="trackList-scroll">
			<table className="ui selectable single line very basic compact table trackList">
				<thead>
					<tr>
						<th className="controls">&nbsp;</th>
						<th className="number">{t('tracklist.number')}</th>
						<th className="artist">{t('tracklist.artist')}</th>
						<th className="title">{t('tracklist.title')}</th>
						<th className="album">{t('tracklist.album')}</th>
						<th className="date">{t('tracklist.date')}</th>
						<th className="right aligned duration">{t('tracklist.duration')}</th>
					</tr>
				</thead>
				<tbody>
					{tracks}
				</tbody>
			</table>
			</div>
		);
	}
}

class Track extends Component {
	constructor(props, context) {
		super(props, context);

		this.play = this.play.bind(this);
		this.enqueue = this.enqueue.bind(this);
		this.playlistAdd = this.playlistAdd.bind(this);
		this.playlistRemove = this.playlistRemove.bind(this);
	}

	play() {
		if('queue_sid' in this.props.track){//已经在queue列表中
			this.props.events.publish({event: "playtrack", data: {queue_sid: this.props.track.queue_sid}});
		}else{//在playlist中加入queue并且播放加入的歌曲
			this.props.events.publish({event: "playerEnqueue", data: {action: "ADDPLAY", tracks: [this.props.track]}});
		}
	}

	enqueue() {
		if('queue_sid' in this.props.track){//在queue中删除该歌曲
			this.props.events.publish({event: "playerEnqueue", data: {action: "DEL", tracks: this.props.track}});
		}else{//在playlist中加入queue
			this.props.events.publish({event: "playerEnqueue", data: {action: "ADD", tracks: [this.props.track]}});
		}		
	}

	playlistAdd() {
		this.props.events.publish({event: "playlistManage", data: {action: "ADD", tracks: [this.props.track]}});
	}

	playlistRemove() {
		this.props.events.publish({event: "playlistManage", data: {action: "REMOVE", tracks: [this.props.track], id: this.props.playlist}});
	}

	render() {
		var playlistButton;
		if (this.props.playlist) {
			playlistButton = (
				<button className="ui mini compact icon teal button" title={t('tracklist.removeFromPlaylist')} onClick={this.playlistRemove}>
					<i className="minus icon"></i>
				</button>
			);
		} else {
			playlistButton = (
				<button className="ui mini compact icon teal button" title={t('tracklist.addToPlaylist')} onClick={this.playlistAdd}>
					<i className="list icon"></i>
				</button>
			);
		}

		return (
			<tr className={this.props.playing ? "positive" : ""}>
				<td className="controls">
					<button className="ui mini compact icon green button" onClick={this.play} title={t('tracklist.playNow')}><i className="play icon"></i></button>
					<button className="ui mini compact icon olive button" onClick={this.enqueue} title={this.props.queued ? t('tracklist.removeFromQueue') : t('tracklist.addToQueue')}>
						<i className={this.props.queued ? "minus icon" : "plus icon"}></i>
					</button>
					{playlistButton}
				</td>
				<td>
					{this.props.track.discNumber ? (this.props.track.discNumber + '.' + this.props.track.track) : this.props.track.track}
				</td>
				<td>
					{this.props.track.artist}
				</td>
				<td>
					{this.props.track.title}
				</td>
				<td>
					{/* <CoverArt subsonic={this.props.subsonic} id={this.props.track.coverArt} size={this.props.iconSize} /> */}
					{this.props.track.album}
				</td>
				<td>
					{this.props.track.year}
				</td>
				<td className="right aligned">
					{this.props.track.duration ? SecondsToTime(this.props.track.duration) : '?:??'}
				</td>
			</tr>
		);
	}
}
