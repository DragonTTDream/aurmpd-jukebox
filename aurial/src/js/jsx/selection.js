import { h, Component } from 'preact';
import moment from 'moment'
import {IconMessage,CoverArt} from './common'
import TrackList from './tracklist'
import {t} from '../i18n'
import {SecondsToTime} from '../util'

export default class Selection extends Component {
	state = {
		album: null
	}

	constructor(props, context) {
		super(props, context);
		props.events.subscribe({
			subscriber: this,
			event: ["browserSelected"]
		});
	}

	receive(event) {
		switch (event.event) {
			case "browserSelected": this.setState({album: event.data.tracks});break;	
		}
		
	}

	render() {
		if (this.state.album == null) {
			return (
				<IconMessage icon="info circle" header={t('selection.nothingHeader')} message={t('selection.nothingMessage')} />
			);

		} else {
			//console.log("event.data.tracks:"+JSON.stringify(this.state.album));
			return (
				<div className="ui basic segment selectionView">
					<SelectionAlbum subsonic={this.props.subsonic} events={this.props.events} album={this.state.album} />
					<TrackList subsonic={this.props.subsonic} events={this.props.events} tracks={this.state.album.song} iconSize={this.props.iconSize} />
				</div>
			);
		}
	}
}

class SelectionAlbum extends Component {

	constructor(props, context) {
		super(props, context);

		this.play = this.play.bind(this);
		this.enqueue = this.enqueue.bind(this);
		this.playlist = this.playlist.bind(this);
	}

	play() {
		this.props.events.publish({event: "playerEnqueue", data: {action: "REPLACE", tracks: this.props.album.song}});
	}

	enqueue() {
		this.props.events.publish({event: "playerEnqueue", data: {action: "ADD", tracks: this.props.album.song}});
	}

	playlist() {
		this.props.events.publish({event: "playlistManage", data: {action: "ADD", tracks: this.props.album.song}});
	}

	render() {
		return (
			<div className="ui items albumInfo">
				<div className="item">
					<div className="ui small image">
						<CoverArt subsonic={this.props.subsonic} id={this.props.album.coverArt} events={this.props.events} size={200} />
					</div>
					<div className="aligned content">
						<div className="header">
							<div className="artist">{this.props.album.artist}</div>
							<div>{this.props.album.name}</div>
						</div>
						<div className="meta">
							<div>{this.props.album.genre != '(255)' ? this.props.album.genre : ""}</div>
							<div>{this.props.album.year ? t('selection.year', {year: this.props.album.year}) : ""}</div>
							<div>{t('selection.added', {date: moment(this.props.album.created).format("ll")})}</div>
							<div>{t('selection.tracks', {count: this.props.album.songCount, duration: SecondsToTime(this.props.album.duration)})}</div>
						</div>
						<div className="extra">
							<button className="ui small compact labelled icon green button" onClick={this.play}><i className="play icon"></i> {t('selection.play')}</button>
							<button className="ui small compact labelled icon olive button" onClick={this.enqueue}><i className="plus icon"></i> {t('selection.addToQueue')}</button>
							<button className="ui small compact labelled icon teal button" onClick={this.playlist}><i className="list icon"></i> {t('selection.addToPlaylist')}</button>
						</div>
					</div>
				</div>
			</div>
		);
	}
}
