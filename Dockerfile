FROM node:20-alpine
WORKDIR /app
COPY server/online-api/package*.json ./
RUN npm install --omit=dev
COPY server/online-api/ ./
ENV NODE_ENV=production
ENV PORT=8080
EXPOSE 8080
CMD ["npm","start"]
